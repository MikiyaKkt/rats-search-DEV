#include "data/query.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace rats::data::sql {

QString escape(const QString& value)
{
    QString result = value;
    result.replace('\\', QLatin1String("\\\\"));
    result.replace('\'', QLatin1String("\\'"));
    result.replace('"', QLatin1String("\\\""));
    result.replace('\n', QLatin1String("\\n"));
    result.replace('\r', QLatin1String("\\r"));
    result.replace('\t', QLatin1String("\\t"));
    result.replace(QChar('\0'), QLatin1String("\\0"));
    return result;
}

QString quote(const QString& value)
{
    return QLatin1Char('\'') + escape(value) + QLatin1Char('\'');
}

static QString jsonToCompactString(const QVariant& value)
{
    if (value.typeId() == QMetaType::QJsonObject)
        return QString::fromUtf8(QJsonDocument(value.toJsonObject()).toJson(QJsonDocument::Compact));
    if (value.typeId() == QMetaType::QJsonArray)
        return QString::fromUtf8(QJsonDocument(value.toJsonArray()).toJson(QJsonDocument::Compact));
    if (value.typeId() == QMetaType::QVariantMap)
        return QString::fromUtf8(
            QJsonDocument(QJsonObject::fromVariantMap(value.toMap())).toJson(QJsonDocument::Compact));
    return QString();
}

QString formatValue(const QVariant& value)
{
    if (value.isNull() || !value.isValid())
        return QStringLiteral("''"); // Manticore has no NULL

    switch (value.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
        return value.toString();
    case QMetaType::QJsonObject:
    case QMetaType::QJsonArray:
    case QMetaType::QVariantMap:
        return quote(jsonToCompactString(value));
    default:
        return quote(value.toString());
    }
}

QString escapeMatch(const QString& userQuery)
{
    // Manticore extended full-text operators. '*' is intentionally excluded so
    // prefix/wildcard search keeps working. Backslash is escaped first so we do
    // not double-escape the escapes we add.
    static const QString specials = QStringLiteral("\\!\"$()-/<@^|~=&");
    QString result;
    result.reserve(userQuery.size() + 8);
    for (QChar c : userQuery) {
        if (specials.contains(c))
            result += QLatin1Char('\\');
        result += c;
    }
    return result;
}

QString substitute(const QString& sql, const QVariantList& params)
{
    if (params.isEmpty())
        return sql;

    QString result;
    result.reserve(sql.size() + params.size() * 32);
    int paramIndex = 0;
    for (QChar c : sql) {
        if (c == QLatin1Char('?') && paramIndex < params.size())
            result += formatValue(params[paramIndex++]);
        else
            result += c;
    }
    return result;
}

bool isIdentifier(const QString& identifier)
{
    if (identifier.isEmpty())
        return false;
    const QChar first = identifier.at(0);
    if (!first.isLetter() && first != QLatin1Char('_'))
        return false;
    for (QChar c : identifier) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            return false;
    }
    return true;
}

} // namespace rats::data::sql

namespace rats::data {

namespace {

const QRegularExpression& continuousScript()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"([\p{Han}\p{Hiragana}\p{Katakana}\p{Hangul}\p{Thai}\x{30FC}]+)"));
    return pattern;
}

bool hasSearchableText(const QString& text)
{
    static const QRegularExpression letterOrNumber(QStringLiteral(R"([\p{L}\p{N}])"));
    return letterOrNumber.match(text).hasMatch();
}

QString quotedPhrase(const QString& text)
{
    return QLatin1Char('"') + sql::escapeMatch(text) + QLatin1Char('"');
}

bool isOneEdit(const QList<uint>& from, const QList<uint>& to, qsizetype start, qsizetype length)
{
    const qsizetype delta = length - from.size();
    if (delta < -1 || delta > 1)
        return false;
    qsizetype i = 0;
    qsizetype j = 0;
    bool edited = false;
    while (i < from.size() && j < length) {
        if (from[i] == to[start + j]) {
            ++i;
            ++j;
        } else {
            if (edited)
                return false;
            edited = true;
            if (delta <= 0)
                ++i;
            if (delta >= 0)
                ++j;
        }
    }
    return edited ? i == from.size() && j == length : delta != 0;
}

} // namespace

FullTextQuery FullTextQuery::parse(const QString& text)
{
    FullTextQuery query;
    const QChar leftQuote(0x201c);
    const QChar rightQuote(0x201d);
    const auto isQuote = [=](QChar c) { return c == u'"' || c == leftQuote || c == rightQuote; };
    const auto addWord = [&query](const QString& word) {
        if (hasSearchableText(word))
            query.terms_.append({ word, Kind::Word, {} });
    };

    for (qsizetype pos = 0; pos < text.size();) {
        if (text[pos].isSpace()) {
            ++pos;
            continue;
        }
        if (isQuote(text[pos])) {
            const QChar opening = text[pos++];
            const QChar closing = opening == leftQuote ? rightQuote : opening;
            const qsizetype end = text.indexOf(closing, pos);
            if (opening == rightQuote || end < 0) {
                query.error_ = Error::UnclosedQuote;
                return query;
            }
            QString phrase = text.mid(pos, end - pos);
            // Manticore expands a wildcard in a phrase even if it is escaped.
            // These characters are separators in the configured tokenizer.
            for (QChar& c : phrase) {
                if (c == u'*' || c == u'?' || c == u'%')
                    c = u' ';
            }
            if (!hasSearchableText(phrase)) {
                query.error_ = Error::EmptyPhrase;
                return query;
            }
            query.terms_.append({ phrase.simplified(), Kind::Phrase, {} });
            query.hasExactPhrase_ = true;
            pos = end + 1;
            continue;
        }

        const qsizetype start = pos;
        while (pos < text.size() && !text[pos].isSpace() && !isQuote(text[pos]))
            ++pos;
        const QString word = text.mid(start, pos - start);
        const bool wildcard = word.contains(u'*') || word.contains(u'?') || word.contains(u'%');
        qsizetype end = 0;
        for (const auto& match : continuousScript().globalMatch(word)) {
            addWord(word.mid(end, match.capturedStart() - end));
            const QString run = match.captured();
            query.terms_.append({ run, Kind::Continuous, {}, wildcard });
            if (!query.continuousTerms_.contains(run))
                query.continuousTerms_.append(run);
            end = match.capturedEnd();
        }
        addWord(word.mid(end));
    }
    return query;
}

bool FullTextQuery::allowsShortQuery(const QString& text)
{
    const auto match = continuousScript().match(text.trimmed());
    return match.hasMatch() && match.captured().toUcs4().size() >= 2;
}

QVector<FullTextQuery::FuzzyTerm> FullTextQuery::fuzzyTerms() const
{
    static const QRegularExpression english(QStringLiteral("^[a-zA-Z]+$"));
    QVector<FuzzyTerm> candidates;
    for (qsizetype i = 0; error_ == Error::None && i < terms_.size(); ++i) {
        const auto& term = terms_[i];
        if (term.wildcard)
            continue;
        const bool continuous = term.kind == Kind::Continuous;
        if (!continuous && (term.kind != Kind::Word || !english.match(term.text).hasMatch()))
            continue;
        const qsizetype length = term.text.toUcs4().size();
        if (length >= 4 && length <= 24)
            candidates.append({ i, term.text.toLower(), continuous });
    }
    return candidates;
}

QStringList FullTextQuery::FuzzyTerm::anchors() const
{
    if (continuous) {
        // A single edit leaves at least one half of the original run intact.
        qsizetype half = text.size() / 2;
        if (text.at(half).isLowSurrogate())
            ++half;
        return { text.left(half), text.mid(half) };
    }

    // Enumerate exactly one edit over the English alphabet. The engine looks
    // these words up by dictionary key, including corrections at position 0.
    QSet<QString> edits;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const QString prefix = text.left(i);
        const QString suffix = text.mid(i);
        if (i < text.size())
            edits.insert(prefix + suffix.mid(1));
        for (char16_t letter = u'a'; letter <= u'z'; ++letter) {
            edits.insert(prefix + QChar(letter) + suffix);
            if (i < text.size() && letter != text[i])
                edits.insert(prefix + QChar(letter) + suffix.mid(1));
        }
    }
    auto ordered = edits.values();
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

bool FullTextQuery::FuzzyTerm::matches(const QString& source) const
{
    const auto original = text.toUcs4();
    static const QRegularExpression words(QStringLiteral(R"([\p{L}\p{M}\p{N}_]+)"));
    if (!continuous) {
        // Han and other continuous runs are separate tokens in this index,
        // even next to Latin letters in the original title or path.
        QString separated = source;
        separated.replace(continuousScript(), QStringLiteral(" "));
        for (const auto& match : words.globalMatch(separated)) {
            const auto word = match.captured().toLower().toUcs4();
            if (isOneEdit(original, word, 0, word.size()))
                return true;
        }
        return false;
    }
    for (const auto& match : continuousScript().globalMatch(source)) {
        const auto run = match.captured().toUcs4();
        for (qsizetype start = 0; start + original.size() - 1 <= run.size(); ++start) {
            for (qsizetype length = original.size() - 1; length <= original.size() + 1 && start + length <= run.size(); ++length) {
                if (isOneEdit(original, run, start, length))
                    return true;
            }
        }
    }
    return false;
}

FullTextQuery FullTextQuery::withAlternatives(qsizetype index, const QStringList& alternatives) const
{
    FullTextQuery query = *this;
    query.terms_[index] = { {}, Kind::Alternatives, alternatives };
    query.hasExactPhrase_ = true;
    query.continuousTerms_.clear();
    for (const Term& term : query.terms_) {
        if (term.kind == Kind::Continuous && !query.continuousTerms_.contains(term.text))
            query.continuousTerms_.append(term.text);
    }
    return query;
}

QString FullTextQuery::expression(bool prefixIndex) const
{
    if (error_ != Error::None)
        return {};

    // The title index normally expands unquoted words implicitly. When a
    // quoted phrase disables that option, restore prefixes on ordinary words.
    static const QRegularExpression prefixWords(QStringLiteral(R"([\p{L}\p{N}\p{M}_*?%]+)"));
    QStringList clauses;
    for (const Term& term : terms_) {
        if (term.kind == Kind::Alternatives) {
            QStringList alternatives;
            for (const QString& word : term.alternatives)
                alternatives.append(quotedPhrase(word));
            clauses.append(u'(' + alternatives.join(QStringLiteral(" | ")) + u')');
        } else if (term.kind != Kind::Word) {
            clauses.append(quotedPhrase(term.text));
        } else if (prefixIndex && hasExactPhrase_) {
            for (const auto& match : prefixWords.globalMatch(term.text)) {
                const QString word = match.captured().toLower();
                if (!hasSearchableText(word))
                    continue;
                if (word.toUcs4().size() >= 3 && !word.contains(u'*') && !word.contains(u'?') && !word.contains(u'%'))
                    clauses.append(QStringLiteral("(%1 | %1*)").arg(word));
                else
                    clauses.append(word);
            }
        } else {
            // Lowercase words to prevent uppercase MATCH operators such as
            // MAYBE or SENTENCE from being parsed as operators.
            clauses.append(sql::escapeMatch(term.text.toLower()));
        }
    }
    return clauses.join(QLatin1Char(' '));
}

SelectQuery::SelectQuery(QString table) : table_(std::move(table)) { }

SelectQuery& SelectQuery::columns(const QString& expr)
{
    columns_ = expr;
    return *this;
}

SelectQuery& SelectQuery::matchAgainst(const QString& userQuery)
{
    conditions_ << QStringLiteral("MATCH(%1)").arg(sql::quote(sql::escapeMatch(userQuery)));
    return *this;
}

SelectQuery& SelectQuery::matchAgainst(const FullTextQuery& query, bool prefixIndex)
{
    conditions_ << QStringLiteral("MATCH(%1)").arg(sql::quote(query.expression(prefixIndex)));
    disableKeywordExpansion_ = query.hasExactPhrase();
    return *this;
}

SelectQuery& SelectQuery::whereEq(const QString& column, const QVariant& value)
{
    conditions_ << QStringLiteral("%1 = %2").arg(column, sql::formatValue(value));
    return *this;
}

SelectQuery& SelectQuery::whereInIds(const QString& column, const QVector<qint64>& values)
{
    if (values.isEmpty()) {
        conditions_ << QStringLiteral("1 = 0"); // empty IN matches nothing
        return *this;
    }
    QStringList rendered;
    rendered.reserve(values.size());
    for (qint64 v : values)
        rendered << QString::number(v);
    conditions_ << QStringLiteral("%1 IN (%2)").arg(column, rendered.join(QLatin1String(", ")));
    return *this;
}

SelectQuery& SelectQuery::whereRaw(const QString& condition)
{
    if (!condition.isEmpty())
        conditions_ << condition;
    return *this;
}

SelectQuery& SelectQuery::orderBy(const QString& column, bool descending)
{
    // Defence in depth: silently ignore a column that is not a bare identifier,
    // so a caller mistake can never turn into SQL injection.
    if (sql::isIdentifier(column))
        orderClause_
            = QStringLiteral(" ORDER BY %1 %2").arg(column, descending ? QLatin1String("DESC") : QLatin1String("ASC"));
    return *this;
}

SelectQuery& SelectQuery::limit(int offset, int count)
{
    limitClause_ = QStringLiteral(" LIMIT %1,%2").arg(offset).arg(count);
    return *this;
}

SelectQuery& SelectQuery::withStableOrder()
{
    if (orderClause_.isEmpty())
        orderClause_ = QStringLiteral(" ORDER BY WEIGHT() DESC");
    orderClause_ += QStringLiteral(", id ASC");
    return *this;
}

SelectQuery& SelectQuery::maxMatches(int count)
{
    maxMatches_ = count;
    return *this;
}

SelectQuery& SelectQuery::maxQueryTime(int milliseconds)
{
    maxQueryTime_ = milliseconds;
    return *this;
}

QString SelectQuery::build() const
{
    QString sql = QStringLiteral("SELECT %1 FROM %2").arg(columns_, table_);
    if (!conditions_.isEmpty())
        sql += QStringLiteral(" WHERE ") + conditions_.join(QLatin1String(" AND "));
    sql += orderClause_;
    sql += limitClause_;
    QStringList options;
    if (disableKeywordExpansion_)
        options << QStringLiteral("expand_keywords=0");
    if (maxMatches_ > 0)
        options << QStringLiteral("max_matches=%1").arg(maxMatches_);
    if (maxQueryTime_ > 0)
        options << QStringLiteral("max_query_time=%1").arg(maxQueryTime_);
    if (!options.isEmpty())
        sql += QStringLiteral(" OPTION ") + options.join(QStringLiteral(", "));
    return sql;
}

} // namespace rats::data
