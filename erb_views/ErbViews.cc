// Type-checks Rails' ERB templates. Sorbet reads each `.erb` file under `app/views/` or `app/components/` (with
// `--allowed-extension=.erb` added to `.rb` and `.rbi`) as the Ruby in its tags:
//
// - `transformSource` gives Sorbet the Ruby to parse: the template's code where it is and everything else as spaces,
//   so every error points at the template's own line and column. Each tag's code ends with a `;` in its closing
//   delimiter (the `>` of `%>`, the `%` of `-%>`), as Erubi (Rails' ERB) separates the code of one tag from the next.
//   The file keeps its ERB, which error messages show and autocorrects edit.
// - A template is `typed: strict` unless a `<%# typed: <level> %>` comment says otherwise (Sorbet finds the sigil in
//   the ERB itself; it needs a space before `%>`).
// - `rewriteClassBody` puts that code in a method, `__render`, with an implied `sig { void }`, on a class named
//   after the template's path: `app/views/templates/show.html.erb` is `SorbetViews::Templates::ShowHtml`,
//   `app/views/templates/_row.html.erb` is `SorbetViews::Templates::RowPartialHtml`, and
//   `app/components/ds/card_component.html.erb` is `SorbetViews::Components::Ds::CardComponentHtml`.
//   Its superclass is the template's context, the same path under `SorbetViews::Contexts`.
//
// generate_rbi.rb writes the contexts from the running app: the view with the helpers of the controllers that
// render the template (or for a component's template, the component's class), its instance variables and its
// locals. A template the RBI doesn't have yet (a new one) fails first on its line 1: its context doesn't resolve.
//
// Other `.erb` files (config, CI and Docker templates) are `typed: ignore`, and read as empty files if a sigil says
// otherwise.
//
// `SORBET_ERB_VIEWS_PRINT=1` prints each template's Ruby to stderr, after `erb_views: <path>` and ending with a NUL.

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::erb_views {

namespace {

constexpr string_view VIEWS_DIR = "app/views/";
constexpr string_view COMPONENTS_DIR = "app/components/";

// The part of `path` after `app/views/` or `app/components/`, and whether it's a component's template.
optional<pair<string_view, bool>> templatePath(string_view path) {
    if (!absl::EndsWith(path, ".erb")) {
        return nullopt;
    }
    for (auto [dir, component] : {pair{VIEWS_DIR, false}, pair{COMPONENTS_DIR, true}}) {
        for (size_t at = path.find(dir); at != string_view::npos; at = path.find(dir, at + 1)) {
            if (at == 0 || path[at - 1] == '/') {
                return pair{path.substr(at + dir.size()), component};
            }
        }
    }
    return nullopt;
}

// Must match `camelize` in generator/names.rb: split on anything but letters and digits, upcase each part's first
// letter.
string camelize(string_view name) {
    string out;
    bool startOfPart = true;
    for (char c : name) {
        if (!isalnum(static_cast<unsigned char>(c))) {
            startOfPart = true;
            continue;
        }
        out += startOfPart ? static_cast<char>(toupper(static_cast<unsigned char>(c))) : c;
        startOfPart = false;
    }
    return out;
}

// Must match `class_parts` in generator/names.rb. The constant path of the class a template's code runs in, without
// `SorbetViews`: a part per directory, then the file's name, with `Partial` after a partial's name and its formats
// after that (`templates/_row.html.erb` => Templates, RowPartialHtml). A part that would start with a digit gets a
// leading `N` (`errors/404.html.erb` => Errors, N404Html).
vector<string> classPath(string_view relative, bool component) {
    vector<string> parts;
    if (component) {
        parts.emplace_back("Components");
    }
    vector<string_view> segments = absl::StrSplit(relative, '/');
    auto file = segments.back();
    segments.pop_back();
    for (auto dir : segments) {
        parts.emplace_back(camelize(dir));
    }
    vector<string_view> pieces = absl::StrSplit(file, '.');
    pieces.pop_back(); // erb
    auto base = pieces.front();
    bool partial = absl::StartsWith(base, "_");
    string name = camelize(base);
    if (partial) {
        name += "Partial";
    }
    for (size_t i = 1; i < pieces.size(); i++) {
        name += camelize(pieces[i]);
    }
    parts.emplace_back(move(name));
    for (auto &part : parts) {
        if (part.empty() || isdigit(static_cast<unsigned char>(part[0]))) {
            part.insert(0, "N");
        }
    }
    return parts;
}

void blank(string &out, string_view source, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        out[i] = source[i] == '\n' ? '\n' : ' ';
    }
}

// The template's Ruby, byte for byte where it was and as long as the template. Tags are Erubi's: `<%= %>` and
// `<%== %>` (output), `<% %>`, `<%- %>` and `-%>`/`=%>` (trim), `<%# %>` (comment), `<%% %>` (literal text).
string templateRuby(string_view source) {
    string out(source.size(), ' ');
    size_t i = 0;
    while (i < source.size()) {
        auto open = source.find("<%", i);
        if (open == string_view::npos) {
            blank(out, source, i, source.size());
            break;
        }
        blank(out, source, i, open);
        auto close = source.find("%>", open + 2);
        if (close == string_view::npos) {
            // Erubi leaves an unclosed tag as text.
            blank(out, source, open, source.size());
            break;
        }
        auto end = close + 2;
        auto codeStart = open + 2;
        char indicator = codeStart < close ? source[codeStart] : '\0';
        if (indicator == '%' || indicator == '#') {
            // `<%% ... %>` is text, and `<%# ... %>` a comment.
            blank(out, source, open, end);
            i = end;
            continue;
        }
        if (indicator == '=') {
            codeStart++;
            if (codeStart < close && source[codeStart] == '=') {
                codeStart++;
            }
        } else if (indicator == '-') {
            codeStart++;
        }
        auto codeEnd = close;
        if (codeEnd > codeStart && (source[codeEnd - 1] == '-' || source[codeEnd - 1] == '=')) {
            codeEnd--;
        }
        blank(out, source, open, codeStart);
        for (size_t c = codeStart; c < codeEnd; c++) {
            out[c] = source[c];
        }
        blank(out, source, codeEnd, end);
        // Where Herb puts it: the closing delimiter's second byte.
        out[codeEnd + 1] = ';';
        i = end;
    }
    return out;
}

class ErbViewsPlugin final : public Plugin {
public:
    string_view name() const override {
        return "erb_views";
    }

    optional<string> transformSource(string_view path, string_view source) const override {
        if (!absl::EndsWith(path, ".erb")) {
            return nullopt;
        }
        if (!templatePath(path).has_value()) {
            string empty(source.size(), ' ');
            blank(empty, source, 0, source.size());
            return empty;
        }
        auto ruby = templateRuby(source);
        // What Sorbet parses in place of the template, for debugging and for scripts/conformance.rb.
        if (getenv("SORBET_ERB_VIEWS_PRINT") != nullptr) {
            fmt::print(stderr, "erb_views: {}\n{}", path, ruby);
            fputc('\0', stderr);
        }
        return ruby;
    }

    optional<core::StrictLevel> strictLevelWithoutSigil(string_view path) const override {
        if (!absl::EndsWith(path, ".erb")) {
            return nullopt;
        }
        return templatePath(path).has_value() ? core::StrictLevel::Strict : core::StrictLevel::Ignore;
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (classDef.symbol != core::Symbols::root()) {
            return;
        }
        auto found = templatePath(ctx.file.data(ctx).path());
        if (!found.has_value()) {
            return;
        }
        auto loc = classDef.loc;
        auto zero = loc.copyWithZeroLength();

        ast::InsSeq::STATS_store stats;
        for (auto &stat : classDef.rhs) {
            stats.emplace_back(move(stat));
        }
        classDef.rhs.clear();
        auto body = stats.empty() ? ast::MK::EmptyTree() : move(stats.back());
        if (!stats.empty()) {
            stats.pop_back();
        }
        auto code = stats.empty() ? move(body) : ast::MK::InsSeq(loc, move(stats), move(body));

        ast::ClassDef::RHS_store classBody;
        // What a template returns is its output, which nothing here checks: an implied `sig { void }`, so templates
        // pass at `typed: strict`.
        classBody.emplace_back(ast::MK::SigVoid(zero, {}));
        classBody.emplace_back(ast::MK::Method0(loc, zero, ctx.state.enterNameUTF8("__render"), move(code)));

        // `::SorbetViews[::Contexts]::<parts>`, at the template's first byte (where Sorbet reports a context that
        // doesn't resolve).
        auto parts = classPath(found->first, found->second);
        auto constant = [&](bool context) {
            auto name = ast::MK::Constant(zero, core::Symbols::root());
            name = ast::MK::UnresolvedConstant(zero, move(name), ctx.state.enterNameConstant("SorbetViews"));
            if (context) {
                name = ast::MK::UnresolvedConstant(zero, move(name), ctx.state.enterNameConstant("Contexts"));
            }
            for (auto &part : parts) {
                name = ast::MK::UnresolvedConstant(zero, move(name), ctx.state.enterNameConstant(part));
            }
            return name;
        };
        ast::ClassDef::ANCESTORS_store ancestors;
        ancestors.emplace_back(constant(true));
        classDef.rhs.emplace_back(ast::MK::Class(loc, zero, constant(false), move(ancestors), move(classBody)));
    }
};

SORBET_PLUGIN(ErbViewsPlugin);

} // namespace

} // namespace sorbet::plugins::erb_views
