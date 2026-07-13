#ifndef CCAFFARCHIVE_H
#define CCAFFARCHIVE_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace cmo3 {

// CAFF 容器（cmo3/can3）读写。大端序，条目级 XOR 混淆 + 可选 ZIP 压缩。
class CCaffArchive
{
public:
    enum CompressOption {
        CompressRaw   = 16,
        CompressFast  = 33,
        CompressSmall = 37,
    };

    struct Entry {
        QString    path;                        // 容器内文件名
        QString    tag;                         // "main_xml" 或空
        QByteArray content;                     // 解压解混淆后的内容
        bool       obfuscated = true;
        int        compress   = CompressRaw;
    };

    bool Load(const QString& filePath, QString* outError = nullptr);
    bool Save(const QString& filePath, QString* outError = nullptr) const;

    void AddEntry(const QString& path, const QByteArray& content,
                  const QString& tag = QString(), int compress = CompressRaw);
    void Clear() { m_entries.clear(); }

    const QVector<Entry>& Entries() const { return m_entries; }
    const Entry* FindByTag(const QString& tag) const;
    const Entry* FindByPath(const QString& path) const;

    qint32 ObfuscateKey() const { return m_key; }
    void SetObfuscateKey(qint32 key) { m_key = key; }

private:
    static QByteArray zipWrap(const QByteArray& content);
    static QByteArray zipUnwrap(const QByteArray& blob, QString* outError);

    QVector<Entry> m_entries;
    qint32 m_key = 0;   // 写出默认 0（合法，等效免混淆）
};

} // namespace cmo3

#endif // CCAFFARCHIVE_H
