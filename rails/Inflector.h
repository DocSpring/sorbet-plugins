#ifndef SORBET_PLUGINS_RAILS_INFLECTOR_H
#define SORBET_PLUGINS_RAILS_INFLECTOR_H

// ActiveSupport's English inflections (activesupport 8.1's `inflections.rb` and `Inflector` methods), with the app's
// own acronyms, irregulars and uncountables from `config/initializers/inflections.rb`: the names Rails gives a
// model's table (`OAuthApplication` → `oauth_applications`) and an association's class (`:entries` → `Entry`).

#include "Source.h"

#include <map>
#include <regex>

namespace sorbet::plugins::rails {

class Inflector {
public:
    // The app's inflections, or an error message if its initializer has an inflection rule the plugin can't read
    // (a regular expression rule, or anything else in an `inflections` block other than `acronym`, `irregular` and
    // `uncountable` with literal strings).
    static Inflector forApp(const App &app, std::string &error);

    std::string pluralize(std::string_view word) const;
    std::string singularize(std::string_view word) const;
    // `OAuthApplication` → `oauth_application`, `Billing::Ledger` → `billing/ledger`.
    std::string underscore(std::string_view word) const;
    // `oauth_application` → `OAuthApplication`, `billing/ledger` → `Billing::Ledger`.
    std::string camelize(std::string_view word) const;

private:
    struct Rule {
        std::regex pattern;
        std::string replacement; // ECMAScript format: `$1`
    };
    // In the order they were defined: the last one that matches wins, as ActiveSupport applies them newest first.
    std::vector<Rule> plurals;
    std::vector<Rule> singulars;
    std::vector<std::string> uncountables;                    // lowercase
    std::map<std::string, std::string, std::less<>> acronyms; // lowercase => as written
    std::vector<std::string> acronymsByLength;                // longest first

    Inflector();
    void acronym(std::string word);
    void irregular(const std::string &singular, const std::string &plural);
    void uncountable(const std::string &word);
    std::string apply(std::string_view word, const std::vector<Rule> &rules) const;
};

} // namespace sorbet::plugins::rails

#endif
