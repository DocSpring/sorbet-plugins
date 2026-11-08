#include "Inflector.h"

#include "absl/strings/ascii.h"
#include "common/common.h"
#include "common/sort/sort.h"

#include <algorithm>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

bool isWordChar(char c) {
    return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool isUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool isLower(char c) {
    return c >= 'a' && c <= 'z';
}

bool isLowerOrDigit(char c) {
    return isLower(c) || (c >= '0' && c <= '9');
}

// Ruby's `\b` at `at`.
bool wordBoundary(string_view text, size_t at) {
    bool before = at > 0 && isWordChar(text[at - 1]);
    bool after = at < text.size() && isWordChar(text[at]);
    return before != after;
}

// Ruby's String#capitalize: the first character upper case, the rest lower case.
string capitalize(string_view word) {
    auto out = absl::AsciiStrToLower(word);
    if (!out.empty()) {
        out[0] = absl::ascii_toupper(static_cast<unsigned char>(out[0]));
    }
    return out;
}

string escapeRegex(string_view text) {
    string out;
    for (char c : text) {
        if (string_view("\\^$.|?*+()[]{}/").find(c) != string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// The rest of an irregular word, matched without regard to case.
string anyCase(string_view text) {
    string out;
    for (char c : text) {
        auto lower = absl::ascii_tolower(static_cast<unsigned char>(c));
        auto upper = absl::ascii_toupper(static_cast<unsigned char>(c));
        if (lower == upper) {
            out += escapeRegex(string(1, c));
        } else {
            out += fmt::format("[{}{}]", lower, upper);
        }
    }
    return out;
}

} // namespace

Inflector::Inflector() {
    // activesupport 8.1 lib/active_support/inflections.rb, with `\1` written as `$1`.
    auto plural = [&](const char *pattern, const char *replacement) {
        plurals.emplace_back(Rule{regex(pattern, regex::ECMAScript | regex::icase), replacement});
    };
    auto singular = [&](const char *pattern, const char *replacement) {
        singulars.emplace_back(Rule{regex(pattern, regex::ECMAScript | regex::icase), replacement});
    };
    plural("$", "s");
    plural("s$", "s");
    plural("^(ax|test)is$", "$1es");
    plural("(octop|vir)us$", "$1i");
    plural("(octop|vir)i$", "$1i");
    plural("(alias|status)$", "$1es");
    plural("(bu)s$", "$1ses");
    plural("(buffal|tomat)o$", "$1oes");
    plural("([ti])um$", "$1a");
    plural("([ti])a$", "$1a");
    plural("sis$", "ses");
    plural("(?:([^f])fe|([lr])f)$", "$1$2ves");
    plural("(hive)$", "$1s");
    plural("([^aeiouy]|qu)y$", "$1ies");
    plural("(x|ch|ss|sh)$", "$1es");
    plural("(matr|vert|ind)(?:ix|ex)$", "$1ices");
    plural("^(m|l)ouse$", "$1ice");
    plural("^(m|l)ice$", "$1ice");
    plural("^(ox)$", "$1en");
    plural("^(oxen)$", "$1");
    plural("(quiz)$", "$1zes");

    singular("s$", "");
    singular("(ss)$", "$1");
    singular("(n)ews$", "$1ews");
    singular("([ti])a$", "$1um");
    singular("((a)naly|(b)a|(d)iagno|(p)arenthe|(p)rogno|(s)ynop|(t)he)(sis|ses)$", "$1sis");
    singular("(^analy)(sis|ses)$", "$1sis");
    singular("([^f])ves$", "$1fe");
    singular("(hive)s$", "$1");
    singular("(tive)s$", "$1");
    singular("([lr])ves$", "$1f");
    singular("([^aeiouy]|qu)ies$", "$1y");
    singular("(s)eries$", "$1eries");
    singular("(m)ovies$", "$1ovie");
    singular("(x|ch|ss|sh)es$", "$1");
    singular("^(m|l)ice$", "$1ouse");
    singular("(bus)(es)?$", "$1");
    singular("(o)es$", "$1");
    singular("(shoe)s$", "$1");
    singular("(cris|test)(is|es)$", "$1is");
    singular("^(a)x[ie]s$", "$1xis");
    singular("(octop|vir)(us|i)$", "$1us");
    singular("(alias|status)(es)?$", "$1");
    singular("^(ox)en", "$1");
    singular("(vert|ind)ices$", "$1ex");
    singular("(matr)ices$", "$1ix");
    singular("(quiz)zes$", "$1");
    singular("(database)s$", "$1");

    irregular("person", "people");
    irregular("man", "men");
    irregular("child", "children");
    irregular("sex", "sexes");
    irregular("move", "moves");
    irregular("zombie", "zombies");

    for (auto word :
         {"equipment", "information", "rice", "money", "species", "series", "fish", "sheep", "jeans", "police"}) {
        uncountable(word);
    }
}

void Inflector::acronym(string word) {
    acronyms[absl::AsciiStrToLower(word)] = word;
    acronymsByLength.clear();
    for (auto &[_, written] : acronyms) {
        acronymsByLength.emplace_back(written);
    }
    // Longest first, as ActiveSupport sorts them (`sort_by { |a| -a.length }`); ties keep the order they were added in
    // Ruby, which only matters for two acronyms of the same length that both match at one place, and can't happen
    // with different words of the same length.
    fast_sort(acronymsByLength,
              [](const string &a, const string &b) { return a.size() != b.size() ? a.size() > b.size() : a < b; });
}

void Inflector::irregular(const string &singularWord, const string &pluralWord) {
    auto erase = [&](const string &word) {
        auto lower = absl::AsciiStrToLower(word);
        uncountables.erase(remove(uncountables.begin(), uncountables.end(), lower), uncountables.end());
    };
    erase(singularWord);
    erase(pluralWord);
    auto s0 = singularWord.substr(0, 1);
    auto srest = singularWord.substr(1);
    auto p0 = pluralWord.substr(0, 1);
    auto prest = pluralWord.substr(1);
    auto add = [](vector<Rule> &rules, const string &pattern, const string &replacement, bool icase) {
        auto flags = icase ? regex::ECMAScript | regex::icase : regex::ECMAScript;
        rules.emplace_back(Rule{regex(pattern, flags), replacement});
    };
    if (absl::AsciiStrToUpper(s0) == absl::AsciiStrToUpper(p0)) {
        add(plurals, "(" + escapeRegex(s0) + ")" + escapeRegex(srest) + "$", "$1" + prest, true);
        add(plurals, "(" + escapeRegex(p0) + ")" + escapeRegex(prest) + "$", "$1" + prest, true);
        add(singulars, "(" + escapeRegex(s0) + ")" + escapeRegex(srest) + "$", "$1" + srest, true);
        add(singulars, "(" + escapeRegex(p0) + ")" + escapeRegex(prest) + "$", "$1" + srest, true);
        return;
    }
    // `/#{s0.upcase}(?i)#{srest}$/`: the first letter as given, the rest in any case.
    auto upper = [](const string &s) { return absl::AsciiStrToUpper(s); };
    auto lower = [](const string &s) { return absl::AsciiStrToLower(s); };
    add(plurals, escapeRegex(upper(s0)) + anyCase(srest) + "$", upper(p0) + prest, false);
    add(plurals, escapeRegex(lower(s0)) + anyCase(srest) + "$", lower(p0) + prest, false);
    add(plurals, escapeRegex(upper(p0)) + anyCase(prest) + "$", upper(p0) + prest, false);
    add(plurals, escapeRegex(lower(p0)) + anyCase(prest) + "$", lower(p0) + prest, false);
    add(singulars, escapeRegex(upper(s0)) + anyCase(srest) + "$", upper(s0) + srest, false);
    add(singulars, escapeRegex(lower(s0)) + anyCase(srest) + "$", lower(s0) + srest, false);
    add(singulars, escapeRegex(upper(p0)) + anyCase(prest) + "$", upper(s0) + srest, false);
    add(singulars, escapeRegex(lower(p0)) + anyCase(prest) + "$", lower(s0) + srest, false);
}

void Inflector::uncountable(const string &word) {
    uncountables.emplace_back(absl::AsciiStrToLower(word));
}

string Inflector::apply(string_view word, const vector<Rule> &rules) const {
    string result(word);
    if (result.empty()) {
        return result;
    }
    // `/\b(?:#{words})\Z/i`
    auto lower = absl::AsciiStrToLower(result);
    for (auto &uncountable : uncountables) {
        if (lower.size() >= uncountable.size() &&
            lower.compare(lower.size() - uncountable.size(), uncountable.size(), uncountable) == 0 &&
            wordBoundary(result, result.size() - uncountable.size())) {
            return result;
        }
    }
    for (auto rule = rules.rbegin(); rule != rules.rend(); ++rule) {
        if (regex_search(result, rule->pattern)) {
            return regex_replace(result, rule->pattern, rule->replacement, regex_constants::format_first_only);
        }
    }
    return result;
}

string Inflector::pluralize(string_view word) const {
    return apply(word, plurals);
}

string Inflector::singularize(string_view word) const {
    return apply(word, singulars);
}

string Inflector::underscore(string_view camelCased) const {
    bool hasUpperOrDash = false;
    for (char c : camelCased) {
        hasUpperOrDash = hasUpperOrDash || isUpper(c) || c == '-';
    }
    if (!hasUpperOrDash && camelCased.find("::") == string_view::npos) {
        return string(camelCased);
    }
    string word;
    for (size_t i = 0; i < camelCased.size(); i++) {
        if (camelCased.substr(i, 2) == "::") {
            word += '/';
            i++;
        } else {
            word += camelCased[i];
        }
    }
    // `/(?:(?<=([A-Za-z\d]))|\b)(#{acronyms})(?=\b|[^a-z])/` → `"#{$1 && '_'}#{$2.downcase}"`
    string marked;
    for (size_t i = 0; i < word.size();) {
        bool afterAlnum = i > 0 && absl::ascii_isalnum(static_cast<unsigned char>(word[i - 1]));
        bool matched = false;
        if (afterAlnum || wordBoundary(word, i)) {
            for (auto &acronym : acronymsByLength) {
                if (word.compare(i, acronym.size(), acronym) != 0) {
                    continue;
                }
                auto end = i + acronym.size();
                if (!wordBoundary(word, end) && !(end < word.size() && !isLower(word[end]))) {
                    continue;
                }
                if (afterAlnum) {
                    marked += '_';
                }
                marked += absl::AsciiStrToLower(acronym);
                i = end;
                matched = true;
                break;
            }
        }
        if (!matched) {
            marked += word[i];
            i++;
        }
    }
    // `/(?<=[A-Z])(?=[A-Z][a-z])|(?<=[a-z\d])(?=[A-Z])/` → "_"
    string out;
    for (size_t i = 0; i < marked.size(); i++) {
        if (i > 0) {
            char before = marked[i - 1];
            char here = marked[i];
            bool next = i + 1 < marked.size() && isLower(marked[i + 1]);
            if ((isUpper(before) && isUpper(here) && next) || (isLowerOrDigit(before) && isUpper(here))) {
                out += '_';
            }
        }
        out += marked[i] == '-' ? '_' : marked[i];
    }
    return absl::AsciiStrToLower(out);
}

string Inflector::camelize(string_view term) const {
    auto acronymOr = [&](string_view word) {
        auto found = acronyms.find(word);
        return found != acronyms.end() ? found->second : capitalize(word);
    };
    bool allLowerOrDigit = true;
    for (char c : term) {
        allLowerOrDigit = allLowerOrDigit && isLowerOrDigit(c);
    }
    if (allLowerOrDigit) {
        return acronymOr(term);
    }
    // `sub(/^[a-z\d]*/)`
    size_t first = 0;
    while (first < term.size() && isLowerOrDigit(term[first])) {
        first++;
    }
    string out = acronymOr(term.substr(0, first));
    // `gsub!(/(?:_|(\/))([a-z\d]*)/i)`
    for (size_t i = first; i < term.size();) {
        char c = term[i];
        if (c != '_' && c != '/') {
            out += c;
            i++;
            continue;
        }
        size_t end = i + 1;
        while (end < term.size() && absl::ascii_isalnum(static_cast<unsigned char>(term[end]))) {
            end++;
        }
        auto word = term.substr(i + 1, end - i - 1);
        if (c == '/') {
            out += "::";
        }
        out += acronymOr(word);
        i = end;
    }
    return out;
}

Inflector Inflector::forApp(const App &app, string &error) {
    Inflector inflector;
    auto path = app.find("config/initializers/inflections.rb");
    if (!path) {
        return inflector;
    }
    auto file = RubyFile::read(*path);
    if (!file) {
        return inflector;
    }
    auto fail = [&](const pm_node_t *node, string_view what) {
        if (error.empty()) {
            error = fmt::format("can't read config/initializers/inflections.rb (line {}): {}. It "
                                "reads `acronym`, `irregular` and `uncountable` with literal strings",
                                file->line(node), what);
        }
    };
    auto literal = [&](const pm_node_t *node) -> optional<string> {
        if (!PM_NODE_TYPE_P(node, PM_STRING_NODE)) {
            return nullopt;
        }
        return stringValue(reinterpret_cast<const pm_string_node_t *>(node)->unescaped);
    };
    for (auto stat : file->topLevel()) {
        if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
            continue;
        }
        auto call = reinterpret_cast<const pm_call_node_t *>(stat);
        if (callName(*file, call) != "inflections" || call->block == nullptr ||
            !PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
            continue;
        }
        auto block = reinterpret_cast<const pm_block_node_t *>(call->block);
        auto args = arguments(call);
        if (args.size() > 1 ||
            (args.size() == 1 &&
             !(PM_NODE_TYPE_P(args[0], PM_SYMBOL_NODE) &&
               stringValue(reinterpret_cast<const pm_symbol_node_t *>(args[0])->unescaped) == "en"))) {
            continue; // another locale's
        }
        pm_constant_id_t param = 0;
        if (block->parameters != nullptr && PM_NODE_TYPE_P(block->parameters, PM_BLOCK_PARAMETERS_NODE)) {
            auto params = reinterpret_cast<const pm_block_parameters_node_t *>(block->parameters)->parameters;
            if (params != nullptr && params->requireds.size == 1 &&
                PM_NODE_TYPE_P(params->requireds.nodes[0], PM_REQUIRED_PARAMETER_NODE)) {
                param = reinterpret_cast<const pm_required_parameter_node_t *>(params->requireds.nodes[0])->name;
            }
        }
        if (param == 0) {
            fail(stat, "an `inflections` block without one block parameter");
            return inflector;
        }
        for (auto rule : statements(block->body)) {
            const pm_call_node_t *ruleCall =
                PM_NODE_TYPE_P(rule, PM_CALL_NODE) ? reinterpret_cast<const pm_call_node_t *>(rule) : nullptr;
            if (ruleCall == nullptr || ruleCall->receiver == nullptr ||
                !PM_NODE_TYPE_P(ruleCall->receiver, PM_LOCAL_VARIABLE_READ_NODE) ||
                reinterpret_cast<const pm_local_variable_read_node_t *>(ruleCall->receiver)->name != param) {
                fail(rule, "a statement that isn't an inflection rule");
                return inflector;
            }
            auto name = callName(*file, ruleCall);
            vector<string> words;
            for (auto arg : arguments(ruleCall)) {
                if (PM_NODE_TYPE_P(arg, PM_ARRAY_NODE) && name == "uncountable") {
                    auto array = reinterpret_cast<const pm_array_node_t *>(arg);
                    for (size_t i = 0; i < array->elements.size; i++) {
                        auto word = literal(array->elements.nodes[i]);
                        if (!word) {
                            fail(rule, "an uncountable word that isn't a literal string");
                            return inflector;
                        }
                        words.emplace_back(move(*word));
                    }
                    continue;
                }
                auto word = literal(arg);
                if (!word) {
                    fail(rule, fmt::format("`{}` with an argument that isn't a literal string", name));
                    return inflector;
                }
                words.emplace_back(move(*word));
            }
            if (name == "acronym" && words.size() == 1) {
                inflector.acronym(words[0]);
            } else if (name == "irregular" && words.size() == 2 && !words[0].empty() && !words[1].empty()) {
                inflector.irregular(words[0], words[1]);
            } else if (name == "uncountable" && !words.empty()) {
                for (auto &word : words) {
                    inflector.uncountable(word);
                }
            } else {
                fail(rule, fmt::format("`{}`", name));
                return inflector;
            }
        }
    }
    return inflector;
}

} // namespace sorbet::plugins::rails
