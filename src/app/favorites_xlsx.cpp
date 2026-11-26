#include "app/favorites_xlsx.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QXmlStreamWriter>

#include <array>
#include <limits>

namespace rats::app {
namespace {

constexpr auto sheetNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr auto relationshipNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr int maxRows = 1048576;
constexpr qint64 maxZip = std::numeric_limits<quint32>::max();

// XLSX uses a ZIP container. Stored entries keep the exporter independent of
// platform zip tools and let large sheets be written from temporary files.
class WorkbookArchive {
public:
    explicit WorkbookArchive(QIODevice& device) : device_(device), stream_(&device)
    {
        stream_.setByteOrder(QDataStream::LittleEndian);
    }

    bool add(QByteArray name, QByteArray bytes)
    {
        QBuffer input(&bytes);
        return input.open(QIODevice::ReadOnly) && add(std::move(name), input);
    }

    bool add(QByteArray name, QIODevice& input)
    {
        const qint64 length = input.size();
        if (length < 0 || device_.pos() + length + 30 + name.size() > maxZip || !input.seek(0))
            return false;
        quint32 crc = 0xffffffffu;
        for (qint64 read = 0; read < length;) {
            const auto block = input.read(128 * 1024);
            if (block.isEmpty())
                return false;
            for (const unsigned char byte : block)
                crc = (crc >> 8) ^ crcTable()[(crc ^ byte) & 0xffu];
            read += block.size();
        }
        if (!input.seek(0))
            return false;

        const Entry entry{ std::move(name), crc ^ 0xffffffffu, quint32(length), quint32(device_.pos()) };
        stream_ << quint32(0x04034b50) << quint16(20);
        writeFields(entry);
        stream_ << quint16(0); // extra-field length
        if (!write(entry.name))
            return false;
        for (qint64 copied = 0; copied < length;) {
            const auto block = input.read(128 * 1024);
            if (block.isEmpty() || !write(block))
                return false;
            copied += block.size();
        }
        entries_.append(entry);
        return stream_.status() == QDataStream::Ok;
    }

    bool finish()
    {
        const qint64 centralOffset = device_.pos();
        for (const auto& entry : entries_) {
            stream_ << quint32(0x02014b50) << quint16(20) << quint16(20);
            writeFields(entry);
            stream_ << quint16(0) << quint16(0) << quint16(0) << quint16(0) << quint32(0) << entry.offset;
            if (!write(entry.name))
                return false;
        }
        if (device_.pos() + 22 > maxZip)
            return false;
        const auto count = quint16(entries_.size());
        const auto centralSize = quint32(device_.pos() - centralOffset);
        stream_ << quint32(0x06054b50) << quint16(0) << quint16(0) << count << count
                << centralSize << quint32(centralOffset) << quint16(0);
        return stream_.status() == QDataStream::Ok;
    }

private:
    struct Entry { QByteArray name; quint32 crc; quint32 length; quint32 offset; };

    void writeFields(const Entry& entry)
    {
        // No compression or extra fields. DOS date is 1980-01-01.
        stream_ << quint16(0) << quint16(0) << quint16(0) << quint16(33)
                << entry.crc << entry.length << entry.length << quint16(entry.name.size());
    }

    bool write(const QByteArray& bytes) { return stream_.writeRawData(bytes.constData(), int(bytes.size())) == bytes.size(); }

    static const std::array<quint32, 256>& crcTable()
    {
        static const auto table = [] {
            std::array<quint32, 256> result{};
            for (quint32 i = 0; i < result.size(); ++i) {
                quint32 value = i;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value >> 1) ^ ((value & 1u) ? 0xedb88320u : 0u);
                result[i] = value;
            }
            return result;
        }();
        return table;
    }

    QIODevice& device_;
    QDataStream stream_;
    QVector<Entry> entries_;
};

QString excelText(const QString& text)
{
    // Preserve literal strings such as "_x000A_" and encode controls that
    // XML 1.0 cannot represent. Inline strings never become formulas.
    static const QRegularExpression special(QStringLiteral("_(?=[xX][0-9a-fA-F]{4}_)|[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x{FFFE}\\x{FFFF}]"));
    QString escaped;
    qsizetype offset = 0;
    auto matches = special.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto start = match.capturedStart();
        escaped += QStringView(text).sliced(offset, start - offset);
        const auto hex = QString::number(uint(text.at(start).unicode()), 16).rightJustified(4, u'0').toUpper();
        escaped += QStringLiteral("_x") + hex + u'_';
        offset = start + match.capturedLength();
    }
    return offset ? escaped + QStringView(text).sliced(offset) : text;
}

class Sheet {
public:
    bool open(const QList<int>& widths)
    {
        if (!file.open())
            return false;
        xml.writeStartDocument();
        xml.writeStartElement("worksheet");
        xml.writeDefaultNamespace(QLatin1String(sheetNs));
        xml.writeStartElement("sheetViews");
        xml.writeStartElement("sheetView");
        xml.writeAttribute("workbookViewId", "0");
        xml.writeEmptyElement("pane");
        xml.writeAttribute("ySplit", "1");
        xml.writeAttribute("topLeftCell", "A2");
        xml.writeAttribute("activePane", "bottomLeft");
        xml.writeAttribute("state", "frozen");
        xml.writeEndElement();
        xml.writeEndElement();
        xml.writeStartElement("cols");
        for (qsizetype i = 0; i < widths.size(); ++i) {
            xml.writeEmptyElement("col");
            const auto column = QString::number(i + 1);
            xml.writeAttribute("min", column);
            xml.writeAttribute("max", column);
            xml.writeAttribute("width", QString::number(widths[i]));
            xml.writeAttribute("customWidth", "1");
        }
        xml.writeEndElement();
        xml.writeStartElement("sheetData");
        return true;
    }

    bool row(const QStringList& cells, bool numericLast = false)
    {
        if (++rows_ > maxRows)
            return false;
        xml.writeStartElement("row");
        const auto index = QString::number(rows_);
        xml.writeAttribute("r", index);
        for (qsizetype i = 0; i < cells.size(); ++i) {
            if (cells[i].size() > 32767)
                return false;
            xml.writeStartElement("c");
            xml.writeAttribute("r", QChar(char16_t(u'A' + i)) + index);
            if (numericLast && i == cells.size() - 1 && cells[i].size() <= 15) {
                xml.writeTextElement("v", cells[i]);
                xml.writeEndElement();
                continue;
            }
            xml.writeAttribute("t", "inlineStr");
            xml.writeStartElement("is");
            xml.writeStartElement("t");
            xml.writeAttribute("xml:space", "preserve");
            xml.writeCharacters(excelText(cells[i]));
            xml.writeEndElement();
            xml.writeEndElement();
            xml.writeEndElement();
        }
        xml.writeEndElement();
        columns_ = cells.size();
        return !xml.hasError();
    }

    bool finish()
    {
        xml.writeEndElement();
        xml.writeEmptyElement("autoFilter");
        xml.writeAttribute("ref", QStringLiteral("A1:%1%2").arg(QChar(char16_t(u'A' + columns_ - 1))).arg(rows_));
        xml.writeEndDocument();
        return !xml.hasError() && file.flush();
    }

    QTemporaryFile file;

private:
    QXmlStreamWriter xml{ &file };
    int rows_ = 0;
    qsizetype columns_ = 0;
};

bool writeParts(WorkbookArchive& archive, bool files)
{
    const QByteArray header = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    const QByteArray types = header
        + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
          "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
          "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
          "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
          "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        + (files ? "<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>" : "")
        + "</Types>";
    const QByteArray relationships = header
        + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    const auto rel = [](const char* id, const char* kind, const char* target) {
        return QByteArray("<Relationship Id=\"") + id + "\" Type=\"" + relationshipNs + '/' + kind
            + "\" Target=\"" + target + "\"/>";
    };
    const QByteArray workbook = header + "<workbook xmlns=\"" + sheetNs + "\" xmlns:r=\"" + relationshipNs
        + "\"><bookViews><workbookView/></bookViews><sheets><sheet name=\"Favorites\" sheetId=\"1\" r:id=\"sheet1\"/>"
        + (files ? "<sheet name=\"Files\" sheetId=\"2\" r:id=\"sheet2\"/>" : "") + "</sheets></workbook>";
    return archive.add("[Content_Types].xml", types)
        && archive.add("_rels/.rels", relationships + rel("workbook", "officeDocument", "xl/workbook.xml") + "</Relationships>")
        && archive.add("xl/workbook.xml", workbook)
        && archive.add("xl/_rels/workbook.xml.rels", relationships + rel("sheet1", "worksheet", "worksheets/sheet1.xml")
            + (files ? rel("sheet2", "worksheet", "worksheets/sheet2.xml") : QByteArray()) + "</Relationships>");
}

} // namespace

QString exportFavoritesXlsx(const QString& path, const QVector<FavoritesStore::Entry>& entries,
    FavoritesStore::ExportOptions options, const FavoritesStore::FileLookup& lookup,
    const std::function<bool()>& cancelled)
{
    const auto message = [](const char* source) { return QCoreApplication::translate("FavoritesWidget", source); };
    Sheet favorites;
    Sheet files;
    QStringList columns{ message("Title"), message("Magnet link"), message("Info hash"), message("Folder") };
    QList<int> widths{ 45, 62, 42, 24 };
    if (options.includeDate) {
        columns.append(message("Added to Favorites (UTC)"));
        widths.append(30);
    }
    if (options.includeFiles) {
        columns.append(message("File list"));
        widths.append(17);
    }
    if (!favorites.open(widths) || !favorites.row(columns)
        || (options.includeFiles && (!files.open({ 42, 70, 19 })
            || !files.row({ message("Info hash"), message("File path"), message("Bytes") }))))
        return message("Could not write the Excel worksheet.");

    const auto limitError = message("Could not write an Excel row. The workbook may exceed Excel's row or cell limits.");
    for (const auto& entry : entries) {
        if (cancelled())
            return message("Export cancelled.");
        const auto& torrent = entry.torrent;
        QStringList values{ torrent.name, torrent.magnetLink(), torrent.hash, entry.folder };
        if (options.includeDate)
            values.append(entry.favoritedAt.toUTC().toString(Qt::ISODateWithMs));
        if (options.includeFiles) {
            const auto list = !torrent.fileList.isEmpty() && torrent.fileList.size() >= torrent.files
                ? torrent.fileList : lookup(torrent.hash);
            for (const auto& item : list) {
                if (cancelled())
                    return message("Export cancelled.");
                if (!files.row({ torrent.hash, item.path, QString::number(item.size) }, true))
                    return limitError;
            }
            values.append(list.isEmpty() ? message("Unavailable") : QString::number(list.size()));
        }
        if (!favorites.row(values))
            return limitError;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
        return output.errorString();
    WorkbookArchive archive(output);
    if (!favorites.finish() || (options.includeFiles && !files.finish()) || !writeParts(archive, options.includeFiles)
        || !archive.add("xl/worksheets/sheet1.xml", favorites.file)
        || (options.includeFiles && !archive.add("xl/worksheets/sheet2.xml", files.file))
        || !archive.finish())
        return message("Could not write the Excel workbook.");
    if (cancelled())
        return message("Export cancelled.");
    return output.commit() ? QString() : output.errorString();
}

} // namespace rats::app
