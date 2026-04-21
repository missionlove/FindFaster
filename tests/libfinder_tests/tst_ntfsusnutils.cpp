#include <QtTest>
#include <cstddef>
#include <cstring>

#include "ntfsusn_utils.h"

class NtfsUsnUtilsTest : public QObject
{
    Q_OBJECT

private slots:
    void buildPath_restoresHierarchy();
    void removeRecordsUnderPath_removesOnlySubtree();
    void formatChannelStatus_containsStateAndTime();
#ifdef Q_OS_WIN
    void decodeRecord_v2_parsesNameAndReason();
#if defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_WIN8)
    void decodeRecord_v3_parsesNameAndReason();
#endif
#endif
};

void NtfsUsnUtilsTest::buildPath_restoresHierarchy()
{
    QHash<quint64, NtfsUsnUtils::Node> nodes;
    NtfsUsnUtils::Node dir;
    dir.frn = 10;
    dir.parentFrn = 1;
    dir.name = QStringLiteral("docs");
    dir.isDirectory = true;

    NtfsUsnUtils::Node file;
    file.frn = 11;
    file.parentFrn = 10;
    file.name = QStringLiteral("readme.txt");
    file.isDirectory = false;

    nodes.insert(dir.frn, dir);
    nodes.insert(file.frn, file);

    const QString path = NtfsUsnUtils::buildPath(QStringLiteral("C:/"), nodes, 11);
    QCOMPARE(path, QStringLiteral("C:/docs/readme.txt"));
}

void NtfsUsnUtilsTest::removeRecordsUnderPath_removesOnlySubtree()
{
    QHash<QString, int> records;
    records.insert(QStringLiteral("C:/a/1.txt"), 1);
    records.insert(QStringLiteral("C:/a/sub/2.txt"), 2);
    records.insert(QStringLiteral("C:/b/3.txt"), 3);

    const int removed = NtfsUsnUtils::removeRecordsUnderPath(&records, QStringLiteral("C:/a"));
    QCOMPARE(removed, 2);
    QCOMPARE(records.size(), 1);
    QVERIFY(records.contains(QStringLiteral("C:/b/3.txt")));
}

void NtfsUsnUtilsTest::formatChannelStatus_containsStateAndTime()
{
    const QDateTime ts = QDateTime::fromString(QStringLiteral("2026-04-16T14:00:00"), Qt::ISODate);
    const QString text = NtfsUsnUtils::formatChannelStatus(QStringLiteral("healthy"),
                                                           QStringLiteral("incremental-applied"),
                                                           ts);
    QVERIFY(text.contains(QStringLiteral("healthy")));
    QVERIFY(text.contains(QStringLiteral("incremental-applied")));
    QVERIFY(text.contains(QStringLiteral("14:00:00")));
}

#ifdef Q_OS_WIN
void NtfsUsnUtilsTest::decodeRecord_v2_parsesNameAndReason()
{
    const QString fileName = QStringLiteral("demo.txt");
    const int nameBytes = fileName.size() * static_cast<int>(sizeof(WCHAR));
    const int recordSize = static_cast<int>(sizeof(USN_RECORD_V2) + nameBytes);

    QByteArray buffer(recordSize, 0);
    USN_RECORD_V2 *record = reinterpret_cast<USN_RECORD_V2 *>(buffer.data());
    record->RecordLength = recordSize;
    record->MajorVersion = 2;
    record->MinorVersion = 0;
    record->FileReferenceNumber = 100;
    record->ParentFileReferenceNumber = 10;
    record->Reason = USN_REASON_FILE_CREATE;
    record->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    record->FileNameLength = static_cast<WORD>(nameBytes);
    memcpy(record->FileName, reinterpret_cast<const WCHAR *>(fileName.utf16()), static_cast<size_t>(nameBytes));

    const NtfsUsnUtils::DecodedEvent event = NtfsUsnUtils::decodeRecord(reinterpret_cast<const USN_RECORD *>(record));
    QVERIFY(event.valid);
    QCOMPARE(event.frn, static_cast<quint64>(100));
    QCOMPARE(event.parentFrn, static_cast<quint64>(10));
    QCOMPARE(event.name, fileName);
    QCOMPARE(event.reason, static_cast<quint32>(USN_REASON_FILE_CREATE));
}

#if defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_WIN8)
void NtfsUsnUtilsTest::decodeRecord_v3_parsesNameAndReason()
{
    const QString fileName = QStringLiteral("v3-demo.log");
    const int nameBytes = fileName.size() * static_cast<int>(sizeof(WCHAR));
    const int recordSize = static_cast<int>(sizeof(USN_RECORD_V3) + nameBytes);

    QByteArray buffer(recordSize, 0);
    USN_RECORD_V3 *record = reinterpret_cast<USN_RECORD_V3 *>(buffer.data());
    record->RecordLength = recordSize;
    record->MajorVersion = 3;
    record->MinorVersion = 0;

    const quint64 frnValue = 0x1122334455667788ULL;
    const quint64 parentFrnValue = 0x8877665544332211ULL;
    memcpy(&record->FileReferenceNumber, &frnValue, sizeof(quint64));
    memcpy(&record->ParentFileReferenceNumber, &parentFrnValue, sizeof(quint64));

    record->Reason = USN_REASON_RENAME_NEW_NAME;
    record->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V3, FileName));
    record->FileNameLength = static_cast<WORD>(nameBytes);
    memcpy(record->FileName, reinterpret_cast<const WCHAR *>(fileName.utf16()), static_cast<size_t>(nameBytes));

    const NtfsUsnUtils::DecodedEvent event = NtfsUsnUtils::decodeRecord(reinterpret_cast<const USN_RECORD *>(record));
    QVERIFY(event.valid);
    QCOMPARE(event.frn, frnValue);
    QCOMPARE(event.parentFrn, parentFrnValue);
    QCOMPARE(event.name, fileName);
    QCOMPARE(event.reason, static_cast<quint32>(USN_REASON_RENAME_NEW_NAME));
}
#endif
#endif

QTEST_MAIN(NtfsUsnUtilsTest)
#include "tst_ntfsusnutils.moc"
