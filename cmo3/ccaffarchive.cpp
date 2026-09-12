#include "ccaffarchive.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include <QFile>
#include <QtEndian>

namespace cmo3 {

namespace {

// ---- 读取游标：大端 + 条目级 XOR ----
class Cursor
{
public:
    Cursor(const QByteArray& data) : m_data(data) {}

    bool Ok() const { return m_ok; }
    qint64 Pos() const { return m_pos; }
    void SetKey(quint32 key) { m_key = key; }

    QByteArray Raw(int n)
    {
        if (m_pos + n > m_data.size()) { m_ok = false; return QByteArray(); }
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
        if (!m_ok || m_pos + n > static_cast<quint32>(m_data.size())) { m_ok = false; return QString(); }
        QByteArray bytes;
        bytes.reserve(n);
        for (quint32 i = 0; i < n; ++i) bytes.append(static_cast<char>(EU8()));
        return QString::fromUtf8(bytes);
    }

private:
    const QByteArray& m_data;
    qint64 m_pos = 0;
    quint32 m_key = 0;
    bool m_ok = true;
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

} // namespace

bool CCaffArchive::Load(const QString& filePath, QString* outError)
{
    m_entries.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setErr(outError, QString("无法打开文件: %1").arg(filePath));
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    if (data.size() < kHeaderSize + 2 || memcmp(data.constData(), kMagic, 4) != 0) {
        setErr(outError, "不是 CAFF 容器（magic 不匹配）");
        return false;
    }

    Cursor cur(data);
    cur.Raw(4);                       // magic
    cur.Raw(3);                       // archive version
    cur.Raw(4);                       // format id
    cur.Raw(3);                       // format version
    m_key = static_cast<qint32>(cur.U32());
    cur.SetKey(static_cast<quint32>(m_key));
    cur.Raw(8);                       // reserved
    cur.Raw(2);                       // preview format + color
    cur.Raw(2);                       // padding
    cur.Raw(4);                       // preview w/h
    cur.Raw(8);                       // preview start position
    cur.Raw(4);                       // preview file size
    cur.Raw(8);                       // padding

    const quint32 count = cur.EU32();
    if (!cur.Ok() || count > 1000000) {
        setErr(outError, "文件表损坏（条目数不合法）");
        return false;
    }

    struct RawEntry {
        QString path, tag;
        quint32 offset = 0, size = 0;
        bool obfuscated = false;
        int compress = CompressRaw;
    };
    QVector<RawEntry> rawEntries;
    rawEntries.reserve(static_cast<int>(count));

    for (quint32 i = 0; i < count; ++i) {
        RawEntry e;
        e.path = cur.EString();
        e.tag = cur.EString();
        cur.EU32();                   // int64 offset 高 32 位为占位残留，官方只回填低 32 位
        e.offset = cur.EU32();
        e.size = cur.EU32();
        e.obfuscated = cur.EU8() != 0;
        e.compress = cur.EU8();
        cur.Raw(8);                   // padding
        if (!cur.Ok()) {
            setErr(outError, QString("文件表第 %1 项解析失败").arg(i));
            return false;
        }
        rawEntries.append(e);
    }

    const quint8 key8 = static_cast<quint8>(static_cast<quint32>(m_key) & 0xFF);
    for (const RawEntry& raw : rawEntries) {
        if (raw.offset + raw.size > static_cast<quint32>(data.size())) {
            setErr(outError, QString("条目 %1 数据越界").arg(raw.path));
            return false;
        }
        QByteArray blob = data.mid(static_cast<int>(raw.offset), static_cast<int>(raw.size));
        if (raw.obfuscated) {
            char* p = blob.data();
            for (int i = 0; i < blob.size(); ++i) p[i] ^= key8;
        }
        if (raw.compress == CompressFast || raw.compress == CompressSmall) {
            QString zipErr;
            blob = zipUnwrap(blob, &zipErr);
            if (blob.isNull()) {
                setErr(outError, QString("条目 %1 解压失败: %2").arg(raw.path, zipErr));
                return false;
            }
        }
        Entry entry;
        entry.path = raw.path;
        entry.tag = raw.tag;
        entry.content = blob;
        entry.obfuscated = raw.obfuscated;
        entry.compress = raw.compress;
        m_entries.append(entry);
    }
    return true;
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

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setErr(outError, QString("无法写入文件: %1").arg(filePath));
        return false;
    }
    if (file.write(w.Data()) != w.Data().size()) {
        setErr(outError, "写入不完整");
        file.close();
        return false;
    }
    file.close();
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

QByteArray CCaffArchive::zipUnwrap(const QByteArray& blob, QString* outError)
{
    // 官方为流式 ZIP（flags bit3，local header 中 size=0 且无 central directory），
    // miniz 归档 API 依赖 central directory 会失败，直接走 local header + raw inflate
    if (blob.size() < 30 ||
        static_cast<quint8>(blob[0]) != 0x50 || static_cast<quint8>(blob[1]) != 0x4B ||
        static_cast<quint8>(blob[2]) != 0x03 || static_cast<quint8>(blob[3]) != 0x04) {
        setErr(outError, "非 ZIP local header");
        return QByteArray();
    }

    const uchar* p = reinterpret_cast<const uchar*>(blob.constData());
    const quint16 method = qFromLittleEndian<quint16>(p + 8);
    const quint16 nameLen = qFromLittleEndian<quint16>(p + 26);
    const quint16 extraLen = qFromLittleEndian<quint16>(p + 28);
    const int dataStart = 30 + nameLen + extraLen;
    if (dataStart >= blob.size()) {
        setErr(outError, "ZIP local header 越界");
        return QByteArray();
    }

    if (method == 0) {
        return blob.mid(dataStart);
    }
    if (method != 8) {
        setErr(outError, QString("不支持的压缩方式 %1").arg(method));
        return QByteArray();
    }

    // raw deflate 流式解压：块级增长输出缓冲
    tinfl_decompressor decomp;
    tinfl_init(&decomp);

    const mz_uint8* src = p + dataStart;
    size_t srcRemaining = static_cast<size_t>(blob.size() - dataStart);
    QByteArray out;
    size_t dictSize = TINFL_LZ_DICT_SIZE;
    QByteArray dict(static_cast<int>(dictSize), '\0');
    size_t dictOfs = 0;

    for (;;) {
        size_t inBytes = srcRemaining;
        size_t outBytes = dictSize - dictOfs;
        const tinfl_status status = tinfl_decompress(
            &decomp, src, &inBytes,
            reinterpret_cast<mz_uint8*>(dict.data()),
            reinterpret_cast<mz_uint8*>(dict.data()) + dictOfs, &outBytes,
            (srcRemaining ? TINFL_FLAG_HAS_MORE_INPUT : 0));

        src += inBytes;
        srcRemaining -= inBytes;
        out.append(dict.constData() + dictOfs, static_cast<int>(outBytes));
        dictOfs = (dictOfs + outBytes) & (dictSize - 1);

        if (status == TINFL_STATUS_DONE) break;
        if (status < TINFL_STATUS_DONE) {
            setErr(outError, QString("deflate 解压失败 (status=%1)").arg(static_cast<int>(status)));
            return QByteArray();
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && srcRemaining == 0) {
            setErr(outError, "deflate 数据不完整");
            return QByteArray();
        }
    }
    return out;
}

} // namespace cmo3
