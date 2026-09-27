#include "ccaffarchive.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include "std/cexportcontrol.h"
#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QStringConverter>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <QFile>
#include <QSaveFile>
#include <QtEndian>

namespace cmo3 {

namespace {

// ---- 读取游标：大端 + 条目级 XOR ----
class Cursor
{
public:
    Cursor(const QByteArray& data, const CCaffReadLimits& limits)
        : m_data(data), m_stringLimit(limits.maximumStringBytes), m_metadataLeft(limits.maximumMetadataBytes) {}

    bool Ok() const { return m_ok; }
    qint64 Pos() const { return m_pos; }
    void SetKey(quint32 key) { m_key = key; }
    bool ChargeMetadata(qint64 size)
    {
        if (size < 0 || size > m_metadataLeft) return false;
        m_metadataLeft -= size;
        return true;
    }

    QByteArray Raw(int n)
    {
        if (n < 0 || m_pos > m_data.size() || n > m_data.size() - m_pos) { m_ok = false; return QByteArray(); }
        QByteArray r = m_data.mid(static_cast<int>(m_pos), n);
        m_pos += n;
        return r;
    }

    quint8 U8()
    {
        const QByteArray bytes = Raw(1);
        return bytes.isEmpty() ? 0 : static_cast<quint8>(bytes.at(0));
    }
    quint8 EU8() { return U8() ^ static_cast<quint8>(m_key & 0xFF); }

    quint16 U16()
    {
        QByteArray b = Raw(2);
        return m_ok ? qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData())) : 0;
    }

    quint32 U32()
    {
        QByteArray b = Raw(4);
        return m_ok ? qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(b.constData())) : 0;
    }

    quint32 EU32() { return U32() ^ m_key; }

    quint32 EVarint()
    {
        quint32 result = 0;
        int shift = 0;
        for (int i = 0; i < 4; ++i) {
            const quint8 b = EU8();
            result |= static_cast<quint32>(b & 0x7F) << shift;
            if (!(b & 0x80)) return result;
            shift += 7;
        }
        m_ok = false;
        return 0;
    }

    QString EString()
    {
        const quint32 n = EVarint();
        if (!m_ok || n > quint32(m_stringLimit) || m_pos > m_data.size() || n > m_data.size() - m_pos ||
            qint64(n) * 2 > m_metadataLeft) { m_ok = false; return {}; }
        m_metadataLeft -= qint64(n) * 2;
        QByteArray bytes(int(n), Qt::Uninitialized);
        for (quint32 i = 0; i < n; ++i) bytes[int(i)] = static_cast<char>(EU8());
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(bytes);
        if (decoder.hasError() || decoded.contains(QChar::Null)) { m_ok = false; return {}; }
        return decoded;
    }

private:
    const QByteArray& m_data;
    qint64 m_pos = 0;
    quint32 m_key = 0;
    bool m_ok = true;
    int m_stringLimit;
    qint64 m_metadataLeft;
};

// ---- 写出游标：大端 + 条目级 XOR ----
class Writer
{
public:
    void SetKey(quint32 key) { m_key = key; }
    const QByteArray& Data() const { return m_buf; }
    qint64 Pos() const { return m_buf.size(); }

    void Raw(const QByteArray& bytes) { m_buf.append(bytes); }
    void Zeros(int n) { m_buf.append(n, '\0'); }
    void U8(quint8 v)  { m_buf.append(static_cast<char>(v)); }
    void EU8(quint8 v) { U8(v ^ static_cast<quint8>(m_key & 0xFF)); }

    void U16(quint16 v)
    {
        uchar b[2];
        qToBigEndian(v, b);
        m_buf.append(reinterpret_cast<char*>(b), 2);
    }

    void U32(quint32 v)
    {
        uchar b[4];
        qToBigEndian(v, b);
        m_buf.append(reinterpret_cast<char*>(b), 4);
    }

    void EU32(quint32 v) { U32(v ^ m_key); }

    void PatchEU32(qint64 pos, quint32 v)
    {
        uchar b[4];
        qToBigEndian(v ^ m_key, b);
        for (int i = 0; i < 4; ++i) m_buf[static_cast<int>(pos) + i] = static_cast<char>(b[i]);
    }

    void EVarint(quint32 v)
    {
        do {
            quint8 b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            EU8(b);
        } while (v);
    }

    void EString(const QString& s)
    {
        const QByteArray utf8 = s.toUtf8();
        EVarint(static_cast<quint32>(utf8.size()));
        for (char c : utf8) EU8(static_cast<quint8>(c));
    }

    void EBytes(const QByteArray& bytes)
    {
        const quint8 k = static_cast<quint8>(m_key & 0xFF);
        const int base = m_buf.size();
        m_buf.resize(base + bytes.size());
        char* dst = m_buf.data() + base;
        for (int i = 0; i < bytes.size(); ++i) dst[i] = bytes[i] ^ k;
    }

private:
    QByteArray m_buf;
    quint32 m_key = 0;
};

const char kMagic[4] = { 'C', 'A', 'F', 'F' };
const char kFormatId[4] = { '-', '-', '-', '-' };
const quint8 kPreviewNone = 127;
const int kHeaderSize = 0x36;

void setErr(QString* out, const QString& msg)
{
    if (out) *out = msg;
}

bool validLimits(const CCaffReadLimits& limits)
{
    const CCaffReadLimits maximum;
    return limits.maximumFileBytes >= 0 && limits.maximumFileBytes <= maximum.maximumFileBytes &&
           limits.maximumEntryBytes >= 0 && limits.maximumEntryBytes <= maximum.maximumEntryBytes &&
           limits.maximumTotalBytes >= 0 && limits.maximumTotalBytes <= maximum.maximumTotalBytes &&
           limits.maximumMetadataBytes >= 0 && limits.maximumMetadataBytes <= maximum.maximumMetadataBytes &&
           limits.maximumEntries >= 0 && limits.maximumEntries <= maximum.maximumEntries &&
           limits.maximumStringBytes >= 0 && limits.maximumStringBytes <= maximum.maximumStringBytes;
}

} // namespace

bool CCaffArchive::Load(const QString& filePath, QString* outError)
{
    return Load(filePath, outError, nullptr);
}

bool CCaffArchive::Load(const QString& filePath, QString* outError, CExportControl* control,
                        const CCaffReadLimits& limits)
{
    const CCaffReadLimits frozenLimits = limits;
    QByteArray bytes;
    return ReadFileBytes(filePath, bytes, outError, control, frozenLimits) &&
           LoadBytes(bytes, outError, control, frozenLimits);
}

bool CCaffArchive::ReadFileBytes(const QString& filePath, QByteArray& result, QString* outError,
                                 CExportControl* control, const CCaffReadLimits& inputLimits)
{
    const CCaffReadLimits limits = inputLimits;
    if (outError) outError->clear();
    const auto fail = [outError](const QString& message) { setErr(outError, message); return false; };
    if (filePath.isEmpty() || filePath.size() > 32768 || filePath.contains(QChar::Null) ||
        !validLimits(limits))
        return fail(QStringLiteral("CAFF 文件路径或读取预算无效"));
    try {
        const QString path(filePath.constData(), filePath.size());
        const QFileInfo before(path);
        if (!before.isFile()) return fail(QStringLiteral("CAFF 来源不是普通文件"));
        const auto canonicalPath = before.canonicalFilePath();
        const auto pathSize = before.size();
        const auto pathModified = before.lastModified();
        const auto pathMetadata = before.metadataChangeTime();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("无法打开文件：%1").arg(path));
        const qint64 size = file.size();
        const auto modified = file.fileTime(QFileDevice::FileModificationTime);
        const auto metadata = file.fileTime(QFileDevice::FileMetadataChangeTime);
        if (size < 0 || size > limits.maximumFileBytes)
            return fail(QStringLiteral("CAFF 文件超出读取预算"));
        QByteArray bytes;
        while (bytes.size() < size) {
            if (control && control->IsCancellationRequested()) {
                control->Checkpoint();
                return fail(control->Error());
            }
            const auto chunk = file.read(std::min(qint64(1024 * 1024), size - bytes.size()));
            if (chunk.isEmpty()) return fail(QStringLiteral("CAFF 文件读取不完整：%1").arg(file.errorString()));
            bytes.append(chunk);
        }
        const QFileInfo after(path);
        if (file.error() != QFileDevice::NoError || !file.atEnd() || file.size() != size ||
            file.fileTime(QFileDevice::FileModificationTime) != modified ||
            file.fileTime(QFileDevice::FileMetadataChangeTime) != metadata ||
            canonicalPath != after.canonicalFilePath() || pathSize != after.size() ||
            pathModified != after.lastModified() || pathMetadata != after.metadataChangeTime())
            return fail(QStringLiteral("CAFF 文件在读取期间发生变化"));
        if (control && !control->Checkpoint()) return fail(control->Error());
        result.swap(bytes);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("CAFF 文件读取内存不足"));
    } catch (const std::length_error&) {
        return fail(QStringLiteral("CAFF 文件长度超出可表示范围"));
    }
}

bool CCaffArchive::LoadBytes(const QByteArray& input, QString* outError, CExportControl* control,
                             const CCaffReadLimits& inputLimits)
{
    const CCaffReadLimits limits = inputLimits;
    if (outError) outError->clear();
    const auto fail = [outError](const QString& message) { setErr(outError, message); return false; };
    if (!validLimits(limits))
        return fail(QStringLiteral("CAFF 读取预算无效"));
    if (input.size() > limits.maximumFileBytes || input.size() < kHeaderSize + 6)
        return fail(QStringLiteral("CAFF 文件为空、截断或超出读取预算"));
    try {
        const QByteArray data(input.constData(), input.size());
        if (control && !control->Report(CExportControl::Phase::Preparing, 0, 0, QStringLiteral("读取 CAFF 资源")))
            return fail(control->Error());
        if (data.left(4) != QByteArray(kMagic, 4) || data.right(2) != QByteArrayLiteral("bc"))
            return fail(QStringLiteral("CAFF 容器标识或结束标记无效"));
        if (data.mid(4, 3) != QByteArray(3, '\0') || data.mid(7, 4) != QByteArray(kFormatId, 4) ||
            data.mid(11, 3) != QByteArray(3, '\0'))
            return fail(QStringLiteral("不支持此 CAFF 容器版本"));
        Cursor cursor(data, limits);
        cursor.Raw(14);
        const qint32 key = qint32(cursor.U32());
        cursor.SetKey(quint32(key));
        cursor.Raw(36);
        const quint32 count = cursor.EU32();
        if (!cursor.Ok() || count > quint32(limits.maximumEntries) ||
            count > quint64(data.size() - kHeaderSize - 6) / 24 ||
            !cursor.ChargeMetadata(qint64(count) * 64))
            return fail(QStringLiteral("CAFF 条目数量超出文件边界或预算"));
        struct RawEntry {
            QString path, tag;
            quint32 offset = 0, size = 0;
            quint8 obfuscated = 0, compression = 0;
        };
        QVector<RawEntry> rawEntries;
        rawEntries.reserve(int(count));
        QSet<QString> paths;
        for (quint32 index = 0; index < count; ++index) {
            if (control && !control->Checkpoint()) return fail(control->Error());
            RawEntry entry;
            entry.path = cursor.EString();
            entry.tag = cursor.EString();
            cursor.EU32();
            entry.offset = cursor.EU32();
            entry.size = cursor.EU32();
            entry.obfuscated = cursor.EU8();
            entry.compression = cursor.EU8();
            cursor.Raw(8);
            if (!cursor.Ok() || entry.path.isEmpty() || paths.contains(entry.path) || entry.obfuscated > 1 ||
                (entry.compression != CompressRaw && entry.compression != CompressFast && entry.compression != CompressSmall))
                return fail(QStringLiteral("CAFF 条目名称、编码或文件表无效：%1").arg(entry.path));
            paths.insert(entry.path);
            rawEntries.append(std::move(entry));
        }
        QVector<Entry> candidate;
        candidate.reserve(int(count));
        qint64 decodedBytes = 0;
        for (int index = 0; index < rawEntries.size(); ++index) {
            const auto& raw = rawEntries[index];
            if (raw.offset < cursor.Pos() || raw.offset > data.size() - 2 ||
                raw.size > data.size() - 2 - raw.offset)
                return fail(QStringLiteral("CAFF 条目数据越界：%1").arg(raw.path));
            if (control && !control->Report(CExportControl::Phase::Extracting, index, rawEntries.size(), raw.path))
                return fail(control->Error());
            const qint64 remaining = std::min(limits.maximumEntryBytes, limits.maximumTotalBytes - decodedBytes);
            if (raw.compression == CompressRaw && raw.size > remaining)
                return fail(QStringLiteral("CAFF 条目解码总量超出预算"));
            QByteArray blob(data.constData() + raw.offset, raw.size);
            if (raw.obfuscated) {
                const quint8 keyByte = quint8(quint32(key));
                for (qsizetype at = 0; at < blob.size(); at += 65536) {
                    if (control && !control->Checkpoint()) return fail(control->Error());
                    const qsizetype end = std::min(at + 65536, blob.size());
                    for (qsizetype byte = at; byte < end; ++byte) blob[byte] ^= keyByte;
                }
            }
            if (raw.compression != CompressRaw) {
                QByteArray decoded;
                QString error;
                if (!zipUnwrap(blob, decoded, remaining, &error, control))
                    return fail(QStringLiteral("CAFF 条目 %1 解压失败：%2").arg(raw.path, error));
                blob = std::move(decoded);
            }
            decodedBytes += blob.size();
            candidate.append({raw.path, raw.tag, std::move(blob), bool(raw.obfuscated), raw.compression});
        }
        if (control && !control->Report(CExportControl::Phase::Extracting, count, count, QStringLiteral("CAFF 资源读取完成")))
            return fail(control->Error());
        m_entries.swap(candidate);
        m_key = key;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("CAFF 读取内存不足"));
    } catch (const std::length_error&) {
        return fail(QStringLiteral("CAFF 资源长度超出可表示范围"));
    }
}

bool CCaffArchive::Save(const QString& filePath, QString* outError) const
{
    Writer w;
    w.Raw(QByteArray(kMagic, 4));
    w.Zeros(3);                       // archive version 0.0.0
    w.Raw(QByteArray(kFormatId, 4));
    w.Zeros(3);                       // format version 0.0.0
    w.U32(static_cast<quint32>(m_key));
    w.Zeros(8);                       // reserved
    w.U8(kPreviewNone);
    w.U8(kPreviewNone);
    w.Zeros(2);
    w.U16(0);                         // preview width
    w.U16(0);                         // preview height
    w.Zeros(8);                       // preview start position
    w.Zeros(4);                       // preview file size
    w.Zeros(8);                       // padding

    w.SetKey(static_cast<quint32>(m_key));
    w.EU32(static_cast<quint32>(m_entries.size()));

    // 预压缩并记录 offset 占位位置
    QVector<QByteArray> stored;
    QVector<qint64> offsetPatchPos;
    stored.reserve(m_entries.size());
    offsetPatchPos.reserve(m_entries.size());

    for (const Entry& e : m_entries) {
        QByteArray blob = e.content;
        if (e.compress == CompressFast || e.compress == CompressSmall) {
            blob = zipWrap(blob);
            if (blob.isNull()) {
                setErr(outError, QString("条目 %1 压缩失败").arg(e.path));
                return false;
            }
        }
        stored.append(blob);

        w.EString(e.path);
        w.EString(e.tag);
        w.EU32(0);                    // int64 offset 高 32 位
        offsetPatchPos.append(w.Pos());
        w.EU32(0);                    // 低 32 位，稍后回填
        w.EU32(static_cast<quint32>(blob.size()));
        w.EU8(e.obfuscated ? 1 : 0);
        w.EU8(static_cast<quint8>(e.compress));
        w.Zeros(8);
    }

    for (int i = 0; i < stored.size(); ++i) {
        w.PatchEU32(offsetPatchPos[i], static_cast<quint32>(w.Pos()));
        if (m_entries[i].obfuscated) {
            w.EBytes(stored[i]);
        } else {
            w.Raw(stored[i]);
        }
    }

    w.U8(98);
    w.U8(99);

    QSaveFile file(filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setErr(outError, QStringLiteral("无法创建输出文件 %1：%2").arg(filePath, file.errorString()));
        return false;
    }
    const qint64 written = file.write(w.Data());
    if (written != w.Data().size()) {
        const QString reason = file.error() == QFileDevice::NoError ? QStringLiteral("写入不完整") : file.errorString();
        file.cancelWriting();
        setErr(outError, QStringLiteral("写入文件失败 %1（已写入 %2/%3 字节）：%4")
                            .arg(filePath).arg(written).arg(w.Data().size()).arg(reason));
        return false;
    }
    if (!file.commit()) {
        setErr(outError, QStringLiteral("提交文件失败 %1：%2").arg(filePath, file.errorString()));
        return false;
    }
    return true;
}

void CCaffArchive::AddEntry(const QString& path, const QByteArray& content,
                            const QString& tag, int compress)
{
    Entry e;
    e.path = path;
    e.tag = tag;
    e.content = content;
    e.compress = compress;
    m_entries.append(e);
}

const CCaffArchive::Entry* CCaffArchive::FindByTag(const QString& tag) const
{
    for (const Entry& e : m_entries) {
        if (e.tag == tag) return &e;
    }
    return nullptr;
}

const CCaffArchive::Entry* CCaffArchive::FindByPath(const QString& path) const
{
    for (const Entry& e : m_entries) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

QByteArray CCaffArchive::zipWrap(const QByteArray& content)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, content.size() + 1024)) return QByteArray();

    if (!mz_zip_writer_add_mem(&zip, "contents", content.constData(),
                               static_cast<size_t>(content.size()), MZ_DEFAULT_COMPRESSION)) {
        mz_zip_writer_end(&zip);
        return QByteArray();
    }

    void* buf = nullptr;
    size_t bufSize = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &bufSize)) {
        mz_zip_writer_end(&zip);
        return QByteArray();
    }
    QByteArray result(static_cast<const char*>(buf), static_cast<int>(bufSize));
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return result;
}

bool CCaffArchive::zipUnwrap(const QByteArray& blob, QByteArray& result, qint64 maximumBytes,
                              QString* outError, CExportControl* control)
{
    const auto fail = [outError](const QString& message) { setErr(outError, message); return false; };
    if (blob.size() < 30 || maximumBytes < 0) return fail(QStringLiteral("ZIP 文件头截断"));
    const auto* bytes = reinterpret_cast<const uchar*>(blob.constData());
    const auto u16 = [bytes](qsizetype at) { return qFromLittleEndian<quint16>(bytes + at); };
    const auto u32 = [bytes](qsizetype at) { return qFromLittleEndian<quint32>(bytes + at); };
    if (u32(0) != 0x04034b50) return fail(QStringLiteral("非 ZIP local header"));
    const quint16 flags = u16(6), method = u16(8);
    const bool descriptor = flags & 8;
    if ((flags & ~quint16(0x080e)) || (method != 0 && method != 8) || u16(4) > 20)
        return fail(QStringLiteral("不支持此 ZIP 编码"));
    const qsizetype start = 30 + u16(26) + u16(28);
    if (start > blob.size()) return fail(QStringLiteral("ZIP 文件头越界"));
    quint32 crcExpected = u32(14), compressedExpected = u32(18), decodedExpected = u32(22);
    if (!descriptor && (decodedExpected > maximumBytes || compressedExpected > blob.size() - start))
        return fail(QStringLiteral("ZIP 声明长度越界或超出预算"));
    QByteArray output;
    qsizetype consumed = 0;
    quint32 crc = MZ_CRC32_INIT;
    if (method == 0) {
        if (descriptor) return fail(QStringLiteral("不支持缺少长度的未压缩 ZIP 流"));
        if (compressedExpected != decodedExpected) return fail(QStringLiteral("ZIP 未压缩长度不一致"));
        output = QByteArray(blob.constData() + start, decodedExpected);
        consumed = compressedExpected;
        crc = quint32(mz_crc32(crc, reinterpret_cast<const unsigned char*>(output.constData()), output.size()));
    } else {
        tinfl_decompressor decompressor;
        tinfl_init(&decompressor);
        QByteArray dictionary(TINFL_LZ_DICT_SIZE, '\0');
        size_t dictionaryOffset = 0;
        const size_t available = descriptor ? size_t(blob.size() - start) : compressedExpected;
        for (;;) {
            if (control && !control->Checkpoint()) return fail(control->Error());
            size_t inputCount = available - size_t(consumed);
            size_t outputCount = TINFL_LZ_DICT_SIZE - dictionaryOffset;
            const auto status = tinfl_decompress(&decompressor, bytes + start + consumed, &inputCount,
                reinterpret_cast<mz_uint8*>(dictionary.data()),
                reinterpret_cast<mz_uint8*>(dictionary.data()) + dictionaryOffset, &outputCount,
                inputCount ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            consumed += qsizetype(inputCount);
            if (qint64(outputCount) > maximumBytes - output.size())
                return fail(QStringLiteral("ZIP 解码长度超出预算"));
            crc = quint32(mz_crc32(crc, reinterpret_cast<const unsigned char*>(dictionary.constData()) + dictionaryOffset,
                                  outputCount));
            output.append(dictionary.constData() + dictionaryOffset, qsizetype(outputCount));
            dictionaryOffset = (dictionaryOffset + outputCount) & (TINFL_LZ_DICT_SIZE - 1);
            if (status == TINFL_STATUS_DONE) {
                // 旧版 miniz 会预读紧接 deflate 的完整字节。
                consumed -= decompressor.m_num_bits / 8;
                break;
            }
            if (status < TINFL_STATUS_DONE || (!inputCount && !outputCount) ||
                (status == TINFL_STATUS_NEEDS_MORE_INPUT && size_t(consumed) == available))
                return fail(QStringLiteral("ZIP deflate 数据不完整或无效"));
        }
    }
    qsizetype end = start + consumed;
    if (descriptor) {
        if (blob.size() - end >= 4 && u32(end) == 0x08074b50) end += 4;
        if (blob.size() - end < 12) return fail(QStringLiteral("ZIP 数据描述符截断"));
        crcExpected = u32(end);
        compressedExpected = u32(end + 4);
        decodedExpected = u32(end + 8);
        end += 12;
    }
    if (crc != crcExpected || consumed != compressedExpected || output.size() != decodedExpected)
        return fail(QStringLiteral("ZIP 校验和或长度不匹配"));
    if (end != blob.size()) {
        const qsizetype central = end;
        if (blob.size() - end < 46 || u32(end) != 0x02014b50 || u16(end + 8) != flags ||
            u16(end + 10) != method || u32(end + 16) != crc || u32(end + 20) != compressedExpected ||
            u32(end + 24) != decodedExpected || u16(end + 34) != 0 || u32(end + 42) != 0)
            return fail(QStringLiteral("ZIP 中央目录无效"));
        const qsizetype nameLength = u16(end + 28);
        const qsizetype centralLength = 46 + nameLength + u16(end + 30) + u16(end + 32);
        if (centralLength > blob.size() - end || nameLength != u16(26) ||
            blob.mid(end + 46, nameLength) != blob.mid(30, nameLength))
            return fail(QStringLiteral("ZIP 中央目录名称或范围不一致"));
        end += centralLength;
        if (blob.size() - end < 22 || u32(end) != 0x06054b50 || u16(end + 4) || u16(end + 6) ||
            u16(end + 8) != 1 || u16(end + 10) != 1 || u32(end + 12) != centralLength ||
            u32(end + 16) != central || u16(end + 20) != blob.size() - end - 22)
            return fail(QStringLiteral("ZIP 结束目录无效"));
    }
    result.swap(output);
    return true;
}

} // namespace cmo3
