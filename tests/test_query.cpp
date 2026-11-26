/**
 * @file test_query.cpp
 * @brief Unit tests for the rats::data SQL escaping and SelectQuery builder.
 *        Replaces the old test_sphinxql.cpp. Covers the escaping that must NOT
 *        double single quotes (Manticore driver quirk) and the ORDER BY
 *        identifier guard that closes the old injection hole.
 */

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

#include "data/query.h"

using namespace rats::data;

class TestQuery : public QObject {
    Q_OBJECT

private slots:
    // escape()
    void testEscape_simple();
    void testEscape_singleQuote();
    void testEscape_doubleQuote();
    void testEscape_backslash();
    void testEscape_newlineTabCr();
    void testEscape_complex();
    void testEscape_empty();

    // quote()
    void testQuote_simple();
    void testQuote_singleQuote_notDoubled();
    void testQuote_cyrillicWithQuote();

    // formatValue()
    void testFormatValue_int();
    void testFormatValue_longlong();
    void testFormatValue_double();
    void testFormatValue_string();
    void testFormatValue_stringWithQuotes();
    void testFormatValue_null();
    void testFormatValue_jsonObject();
    void testFormatValue_jsonArray();

    // escapeMatch()
    void testEscapeMatch_plain();
    void testEscapeMatch_operators();
    void testEscapeMatch_keepsWildcard();
    void testFullTextSyntax();
    void testFullTextQuoteErrors();
    void testOneEditTerms();

    // substitute()
    void testSubstitute_noParams();
    void testSubstitute_singleString();
    void testSubstitute_intAndString();
    void testSubstitute_quotedJson_notDoubled();
    void testSubstitute_cyrillicJson();

    // isIdentifier()
    void testIsIdentifier();

    // SelectQuery
    void testSelect_all();
    void testSelect_columns();
    void testSelect_whereEq();
    void testSelect_whereInIds();
    void testSelect_whereInIds_empty();
    void testSelect_whereRaw();
    void testSelect_orderByLimit();
    void testSelect_matchAgainst();

    // SelectQuery injection guard
    void testSelect_orderBy_rejectsInjection();
    void testSelect_orderBy_rejectsSpaces();
    void testSelect_orderBy_acceptsIdentifier();
};

// ============================================================================
// escape()
// ============================================================================

void TestQuery::testEscape_simple()
{
    QCOMPARE(sql::escape("hello world"), QString("hello world"));
}

void TestQuery::testEscape_singleQuote()
{
    QCOMPARE(sql::escape("it's a test"), QString("it\\'s a test"));
}

void TestQuery::testEscape_doubleQuote()
{
    QCOMPARE(sql::escape("say \"hello\""), QString("say \\\"hello\\\""));
}

void TestQuery::testEscape_backslash()
{
    QCOMPARE(sql::escape("path\\to\\file"), QString("path\\\\to\\\\file"));
}

void TestQuery::testEscape_newlineTabCr()
{
    QCOMPARE(sql::escape("a\nb"), QString("a\\nb"));
    QCOMPARE(sql::escape("a\tb"), QString("a\\tb"));
    QCOMPARE(sql::escape("a\rb"), QString("a\\rb"));
}

void TestQuery::testEscape_complex()
{
    const QString input = "It's a \"complex\" test\nwith\\special\tchars";
    const QString expected = "It\\'s a \\\"complex\\\" test\\nwith\\\\special\\tchars";
    QCOMPARE(sql::escape(input), expected);
}

void TestQuery::testEscape_empty()
{
    QVERIFY(sql::escape("").isEmpty());
}

// ============================================================================
// quote()
// ============================================================================

void TestQuery::testQuote_simple()
{
    QCOMPARE(sql::quote("hello"), QString("'hello'"));
}

void TestQuery::testQuote_singleQuote_notDoubled()
{
    const QString result = sql::quote("it's test");
    QCOMPARE(result, QString("'it\\'s test'"));
    QVERIFY(!result.contains("''")); // must NOT double quotes
    QVERIFY(result.contains("\\'"));
}

void TestQuery::testQuote_cyrillicWithQuote()
{
    // Regression: Cyrillic filenames with quotes caused parse errors when the
    // driver doubled quotes instead of backslash-escaping.
    const QString result = sql::quote(QString::fromUtf8("Обучение в 1С' -- 2012.pdf"));
    QCOMPARE(result, QString::fromUtf8("'Обучение в 1С\\' -- 2012.pdf'"));
}

// ============================================================================
// formatValue()
// ============================================================================

void TestQuery::testFormatValue_int()
{
    QCOMPARE(sql::formatValue(QVariant(42)), QString("42"));
}

void TestQuery::testFormatValue_longlong()
{
    QCOMPARE(sql::formatValue(QVariant(static_cast<qint64>(9876543210LL))), QString("9876543210"));
}

void TestQuery::testFormatValue_double()
{
    QCOMPARE(sql::formatValue(QVariant(3.14)), QString("3.14"));
}

void TestQuery::testFormatValue_string()
{
    QCOMPARE(sql::formatValue(QVariant(QString("hello"))), QString("'hello'"));
}

void TestQuery::testFormatValue_stringWithQuotes()
{
    QCOMPARE(sql::formatValue(QVariant(QString("it's a 'test'"))), QString("'it\\'s a \\'test\\''"));
}

void TestQuery::testFormatValue_null()
{
    // Manticore has no NULL; null/invalid becomes ''
    QCOMPARE(sql::formatValue(QVariant()), QString("''"));
}

void TestQuery::testFormatValue_jsonObject()
{
    QJsonObject obj;
    obj["name"] = "test's value";
    obj["size"] = 42;
    const QString result = sql::formatValue(QVariant(obj));
    QVERIFY(result.startsWith("'"));
    QVERIFY(result.endsWith("'"));
    QVERIFY(!result.contains("''")); // backslash-escaped, never doubled
}

void TestQuery::testFormatValue_jsonArray()
{
    QJsonArray arr;
    arr.append("a");
    arr.append(2);
    const QString result = sql::formatValue(QVariant(arr));
    QVERIFY(result.startsWith("'"));
    QVERIFY(result.endsWith("'"));
    QVERIFY(result.contains("\"a\"") || result.contains("a"));
}

// ============================================================================
// escapeMatch()
// ============================================================================

void TestQuery::testEscapeMatch_plain()
{
    QCOMPARE(sql::escapeMatch("ubuntu linux"), QString("ubuntu linux"));
}

void TestQuery::testEscapeMatch_operators()
{
    // Extended-query operators must be backslash-escaped.
    const QString result = sql::escapeMatch("(foo | bar) @field");
    QVERIFY(result.contains("\\("));
    QVERIFY(result.contains("\\)"));
    QVERIFY(result.contains("\\|"));
    QVERIFY(result.contains("\\@"));
}

void TestQuery::testEscapeMatch_keepsWildcard()
{
    // '*' is intentionally left intact so prefix search keeps working.
    QCOMPARE(sql::escapeMatch("ubu*"), QString("ubu*"));
}

void TestQuery::testFullTextSyntax()
{
    const auto mixed = FullTextQuery::parse(QStringLiteral("1080p\u3000\u7535\u5f71\tARCHIV"));
    QCOMPARE(mixed.expression(true), QStringLiteral("1080p \"\u7535\u5f71\" archiv"));
    QCOMPARE(mixed.continuousTerms(), QStringList{ QStringLiteral("\u7535\u5f71") });
    QVERIFY(FullTextQuery::allowsShortQuery(QStringLiteral("\u7535\u5f71")));
    QVERIFY(FullTextQuery::allowsShortQuery(QStringLiteral("\u97f3\u697d")));
    QVERIFY(FullTextQuery::allowsShortQuery(QStringLiteral("\uc601\ud654")));
    QVERIFY(FullTextQuery::allowsShortQuery(QStringLiteral("\u0e44\u0e17")));
    QVERIFY(!FullTextQuery::allowsShortQuery(QStringLiteral("ab")));

    const auto phrase = FullTextQuery::parse(QStringLiteral("\u201cUbuntu Linux\u201d archiv u?u*"));
    QCOMPARE(phrase.expression(true), QStringLiteral("\"Ubuntu Linux\" (archiv | archiv*) u?u*"));
    QCOMPARE(phrase.expression(false), QStringLiteral("\"Ubuntu Linux\" archiv u?u*"));
    QVERIFY(SelectQuery("torrents").matchAgainst(phrase, true).limit(0, 7).build().endsWith(
        QStringLiteral("LIMIT 0,7 OPTION expand_keywords=0")));

    QCOMPARE(FullTextQuery::parse(QStringLiteral("\"ubun* linux\" MAYBE")).expression(false),
        QStringLiteral("\"ubun linux\" maybe"));
    const auto operators = FullTextQuery::parse(QStringLiteral("A | @title =B"));
    QCOMPARE(operators.expression(false), QStringLiteral("a \\@title \\=b"));
}

void TestQuery::testFullTextQuoteErrors()
{
    for (const QString& text : { QStringLiteral("\"Ubuntu"), QStringLiteral("\u201cUbuntu"), QStringLiteral("Ubuntu\u201d") }) {
        const auto parsed = FullTextQuery::parse(text);
        QCOMPARE(parsed.error(), FullTextQuery::Error::UnclosedQuote);
        QVERIFY(parsed.expression(true).isEmpty());
    }
    QCOMPARE(FullTextQuery::parse(QStringLiteral("\"\t * \"")).error(), FullTextQuery::Error::EmptyPhrase);
    QVERIFY(FullTextQuery::parse(QStringLiteral("\t\u3000")).isEmpty());
}

void TestQuery::testOneEditTerms()
{
    const auto query = FullTextQuery::parse(QStringLiteral("\"description\" ubntu lin* ab 1080p \u661f\u9645\u7a7f\u60a6"));
    const auto terms = query.fuzzyTerms();
    QCOMPARE(terms.size(), 2);
    const auto& english = terms.front();
    QVERIFY(english.anchors().contains("ubuntu"));
    QVERIFY(english.matches("root/Ubuntu Linux.iso"));
    QVERIFY(english.matches(QStringLiteral("\u7535\u5f71ubuntu")));
    QVERIFY(!english.matches("ubntu Linux"));
    QVERIFY(!english.matches("xubuntu Linux"));
    const auto& chinese = terms.back();
    QCOMPARE(chinese.anchors().size(), 2);
    QVERIFY(chinese.matches(QStringLiteral("\u661f\u9645\u7a7f\u8d8a")));
    QVERIFY(!chinese.matches(QStringLiteral("\u592a\u7a7a\u7a7f\u60a6")));
    QVERIFY(!chinese.matches(QStringLiteral("\u661f\u9645-\u7a7f\u8d8a")));
    QVERIFY(FullTextQuery::parse(QStringLiteral("\u661f\u9645\u7a7f\u60a6*")).fuzzyTerms().isEmpty());
    const auto variant = query.withAlternatives(english.index, { "ubuntu" });
    QVERIFY(variant.expression(false).contains(QStringLiteral("(\"ubuntu\")")));
    QCOMPARE(variant.continuousTerms(), query.continuousTerms());
}

// ============================================================================
// substitute()
// ============================================================================

void TestQuery::testSubstitute_noParams()
{
    QCOMPARE(sql::substitute("SELECT * FROM test", {}), QString("SELECT * FROM test"));
}

void TestQuery::testSubstitute_singleString()
{
    QVariantList params;
    params << QVariant(QString("hello"));
    QCOMPARE(sql::substitute("SELECT * FROM test WHERE name = ?", params),
        QString("SELECT * FROM test WHERE name = 'hello'"));
}

void TestQuery::testSubstitute_intAndString()
{
    QVariantList params;
    params << QVariant(QString("some data")) << QVariant(123);
    QCOMPARE(sql::substitute("INSERT INTO feed (data, id) VALUES (?, ?)", params),
        QString("INSERT INTO feed (data, id) VALUES ('some data', 123)"));
}

void TestQuery::testSubstitute_quotedJson_notDoubled()
{
    // The original bug: JSON data with filenames containing single quotes.
    QVariantList params;
    params << QVariant(QString(R"({"path":"file's name.txt","size":100})")) << QVariant(1);
    const QString result = sql::substitute("INSERT INTO feed (data, id) VALUES (?, ?)", params);
    QVERIFY(result.contains("\\'"));
    QVERIFY(!result.contains("''"));
    QCOMPARE(result,
        QString("INSERT INTO feed (data, id) VALUES ('{\\\"path\\\":\\\"file\\'s name.txt\\\",\\\"size\\\":100}', 1)"));
}

void TestQuery::testSubstitute_cyrillicJson()
{
    const QString jsonData
        = QString::fromUtf8(R"({"files":[{"path":"1C/Кашаев -- 'Обучение в 1С' -- 2012.pdf","size":1144147}]})");
    QVariantList params;
    params << QVariant(jsonData) << QVariant(1);
    const QString result = sql::substitute("INSERT INTO feed (data, id) VALUES (?, ?)", params);
    QVERIFY(!result.contains("''"));
    QVERIFY(result.contains("\\'"));
    QVERIFY(result.startsWith("INSERT INTO feed (data, id) VALUES ('"));
    QVERIFY(result.endsWith(", 1)"));
}

// ============================================================================
// isIdentifier()
// ============================================================================

void TestQuery::testIsIdentifier()
{
    QVERIFY(sql::isIdentifier("seeders"));
    QVERIFY(sql::isIdentifier("content_type"));
    QVERIFY(sql::isIdentifier("_x"));
    QVERIFY(sql::isIdentifier("col9"));

    QVERIFY(!sql::isIdentifier(""));
    QVERIFY(!sql::isIdentifier("9col")); // starts with digit
    QVERIFY(!sql::isIdentifier("seeders DESC")); // contains space
    QVERIFY(!sql::isIdentifier("a; DROP")); // injection attempt
    QVERIFY(!sql::isIdentifier("a-b")); // hyphen
}

// ============================================================================
// SelectQuery
// ============================================================================

void TestQuery::testSelect_all()
{
    QCOMPARE(SelectQuery("torrents").build(), QString("SELECT * FROM torrents"));
}

void TestQuery::testSelect_columns()
{
    QCOMPARE(SelectQuery("torrents").columns("id, hash").build(), QString("SELECT id, hash FROM torrents"));
}

void TestQuery::testSelect_whereEq()
{
    QCOMPARE(SelectQuery("torrents").whereEq("hash", QString("abc")).build(),
        QString("SELECT * FROM torrents WHERE hash = 'abc'"));
}

void TestQuery::testSelect_whereInIds()
{
    // Row ids are rendered bare: they are qint64, so there is nothing to quote,
    // and Manticore only reaches its docid index for an unquoted id.
    const QString sql = SelectQuery("torrents").whereInIds("id", { 7, 4310523244133776513LL }).build();
    QCOMPARE(sql, QString("SELECT * FROM torrents WHERE id IN (7, 4310523244133776513)"));
}

void TestQuery::testSelect_whereInIds_empty()
{
    // Empty IN must match nothing, never produce invalid SQL.
    QCOMPARE(SelectQuery("torrents").whereInIds("id", {}).build(), QString("SELECT * FROM torrents WHERE 1 = 0"));
}

void TestQuery::testSelect_whereRaw()
{
    QCOMPARE(
        SelectQuery("torrents").whereRaw("size > 100").build(), QString("SELECT * FROM torrents WHERE size > 100"));
    // Empty raw condition is ignored.
    QCOMPARE(SelectQuery("torrents").whereRaw("").build(), QString("SELECT * FROM torrents"));
}

void TestQuery::testSelect_orderByLimit()
{
    const QString sql = SelectQuery("torrents").whereRaw("seeders > 0").orderBy("seeders", true).limit(20, 10).build();
    QCOMPARE(sql, QString("SELECT * FROM torrents WHERE seeders > 0 ORDER BY seeders DESC LIMIT 20,10"));
}

void TestQuery::testSelect_matchAgainst()
{
    const QString sql = SelectQuery("torrents").matchAgainst("ubuntu").build();
    QCOMPARE(sql, QString("SELECT * FROM torrents WHERE MATCH('ubuntu')"));
}

// ============================================================================
// SelectQuery injection guard — the key regression
// ============================================================================

void TestQuery::testSelect_orderBy_rejectsInjection()
{
    // A non-identifier ORDER BY column must be silently dropped, never spliced.
    const QString sql = SelectQuery("torrents").orderBy("seeders; DROP TABLE torrents", true).build();
    QVERIFY(!sql.contains("DROP"));
    QVERIFY(!sql.contains("ORDER BY"));
    QCOMPARE(sql, QString("SELECT * FROM torrents"));
}

void TestQuery::testSelect_orderBy_rejectsSpaces()
{
    // "seeders DESC" as a single column is not a bare identifier -> dropped.
    const QString sql = SelectQuery("torrents").orderBy("seeders DESC", false).build();
    QVERIFY(!sql.contains("ORDER BY"));
}

void TestQuery::testSelect_orderBy_acceptsIdentifier()
{
    const QString sql = SelectQuery("torrents").orderBy("added", false).build();
    QCOMPARE(sql, QString("SELECT * FROM torrents ORDER BY added ASC"));
}

QTEST_MAIN(TestQuery)
#include "test_query.moc"
