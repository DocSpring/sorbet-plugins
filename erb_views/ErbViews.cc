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
// - A partial's locals are `__render`'s keyword parameters, from Rails' strict locals comment, typed by a sig comment
//   (each local without a type is `T.untyped`):
//
//     <%# locals: (template:, show_actions: false) -%>
//     <%# sig { params(template: Template, show_actions: T::Boolean) } -%>
//
//   The transform reads them as `__locals=->(template:, show_actions: false){};` and `sig { params(...) };`, and a
//   bare `template` in the template reads the local, as in Rails. `locals: ()` declares none.
// - In a view (not a component's template), a `render` of a literal partial name with literal locals calls a method
//   named after the partial: `render 'templates/row', template: t` and `render partial: 'templates/row', locals:
//   { template: t }` call `__partial__templates__row(template: t)`, which the context declares with the partial's
//   locals as its parameters. So Sorbet checks each local's name and type, and that the partial exists. Other
//   renders (a name in a variable, `collection:`, a block) stay as they are.
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

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "ast/treemap/treemap.h"
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

bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Writes the Ruby for a comment tag that is a magic comment, and returns whether it was one. `<%# locals: (a:, b: 1)
// %>`, Rails' strict locals (its STRICT_LOCALS_REGEX: whitespace after `#` and after `locals:`, then the parameter
// list up to the `)` before the closing delimiter), becomes `__locals=->(a:, b: 1){};`, in place. `<%# sig ... %>`
// becomes the code of a `<% sig ... %>` tag. `close` is where the tag's `%>` starts.
bool magicComment(string &out, string_view source, size_t open, size_t close, size_t end) {
    auto at = open + 3;
    while (at < close && isBlank(source[at])) {
        at++;
    }
    if (at == open + 3) {
        return false;
    }
    auto text = source.substr(at, close - at);
    auto codeEnd = close > at && source[close - 1] == '-' ? close - 1 : close;
    if (absl::StartsWith(text, "locals:")) {
        auto paren = at + 7;
        if (paren >= close || !isBlank(source[paren])) {
            return false;
        }
        while (paren < close && isBlank(source[paren])) {
            paren++;
        }
        auto last = codeEnd;
        while (last > paren && isBlank(source[last - 1])) {
            last--;
        }
        constexpr string_view lambda = "__locals=->";
        // The lambda's arrow goes before the `(` and its `{}` after the `)`, where no newline may be overwritten.
        if (paren >= close || source[paren] != '(' || last <= paren + 1 || source[last - 1] != ')' ||
            paren - open < lambda.size() || end - last < 2 ||
            source.substr(paren - lambda.size(), lambda.size()).find('\n') != string_view::npos ||
            source.substr(last, 2).find('\n') != string_view::npos) {
            return false;
        }
        blank(out, source, open, end);
        out.replace(paren - lambda.size(), lambda.size(), lambda);
        for (auto c = paren; c < last; c++) {
            out[c] = source[c];
        }
        out[last] = '{';
        out[last + 1] = '}';
        if (end - last >= 3 && source[last + 2] != '\n') {
            out[last + 2] = ';';
        }
        return true;
    }
    if (absl::StartsWith(text, "sig ") || absl::StartsWith(text, "sig{")) {
        blank(out, source, open, end);
        for (auto c = at; c < codeEnd; c++) {
            out[c] = source[c];
        }
        out[codeEnd + 1] = ';';
        return true;
    }
    return false;
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
        if (indicator == '#' && magicComment(out, source, open, close, end)) {
            i = end;
            continue;
        }
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

// A partial's locals, from the statements its magic comments became (`__locals = ->(...) {}` and `sig { params(...)
// }`): the lambda's parameters and the sig's name and type pairs.
struct Locals {
    bool declared = false;
    ast::MethodDef::PARAMS_store params;
    ast::Send::ARGS_store types;
};

// Takes the statement into `locals` if a magic comment made it.
bool takeLocals(core::MutableContext ctx, ast::ExpressionPtr &stat, Locals &locals) {
    if (auto assign = ast::cast_tree<ast::Assign>(stat)) {
        auto ident = ast::cast_tree<ast::UnresolvedIdent>(assign->lhs);
        auto lambda = ast::cast_tree<ast::Send>(assign->rhs);
        if (ident == nullptr || ident->kind != ast::UnresolvedIdent::Kind::Local ||
            ident->name != ctx.state.enterNameUTF8("__locals") || lambda == nullptr ||
            lambda->fun != core::Names::lambda() || lambda->block() == nullptr) {
            return false;
        }
        for (auto &param : lambda->block()->params) {
            locals.params.emplace_back(move(param));
        }
        locals.declared = true;
        return true;
    }
    auto sig = ast::cast_tree<ast::Send>(stat);
    if (sig == nullptr || sig->fun != core::Names::sig() || !sig->recv.isSelfReference() || sig->block() == nullptr) {
        return false;
    }
    auto params = ast::cast_tree<ast::Send>(sig->block()->body);
    if (params == nullptr || params->fun != core::Names::params() || params->numPosArgs() != 0 ||
        params->hasKwSplat()) {
        return false;
    }
    for (uint16_t i = 0; i < params->numKwArgs(); i++) {
        locals.types.emplace_back(move(params->getKwKey(i)));
        locals.types.emplace_back(move(params->getKwValue(i)));
    }
    return true;
}

// The sig's params in the order of the method's (Sorbet requires it), `T.untyped` for a local without a type, then
// any types for names that aren't locals, which Sorbet reports.
ast::Send::ARGS_store sigParams(core::MutableContext ctx, Locals &locals) {
    ast::Send::ARGS_store args;
    vector<bool> used(locals.types.size() / 2, false);
    for (auto &param : locals.params) {
        if (ast::isa_tree<ast::BlockParam>(param)) {
            continue;
        }
        auto name = ast::MK::arg2Name(param);
        if (name.kind() == core::NameKind::UNIQUE) {
            continue; // an anonymous `*` or `**`
        }
        bool typed = false;
        for (size_t i = 0; i < used.size(); i++) {
            auto key = ast::cast_tree<ast::Literal>(locals.types[i * 2]);
            if (!used[i] && key != nullptr && key->isSymbol() && key->asSymbol() == name) {
                args.emplace_back(move(locals.types[i * 2]));
                args.emplace_back(move(locals.types[i * 2 + 1]));
                used[i] = typed = true;
                break;
            }
        }
        if (!typed) {
            auto loc = param.loc();
            args.emplace_back(ast::MK::Symbol(loc, name));
            args.emplace_back(ast::MK::Untyped(loc));
        }
    }
    for (size_t i = 0; i < used.size(); i++) {
        if (!used[i]) {
            args.emplace_back(move(locals.types[i * 2]));
            args.emplace_back(move(locals.types[i * 2 + 1]));
        }
    }
    return args;
}

// `templates/row` => `__partial__templates__row`, or nothing for a name that isn't a plain path.
optional<string> partialMethod(string_view name) {
    if (name.empty() || name.front() == '/' || name.back() == '/' || absl::StrContains(name, "//")) {
        return nullopt;
    }
    string method = "__partial__";
    for (char c : name) {
        if (c == '/') {
            method += "__";
        } else if (isalnum(static_cast<unsigned char>(c)) || c == '_') {
            method += c;
        } else {
            return nullopt;
        }
    }
    return method;
}

// Reads of a partial's locals. Rails makes them the parameters of the template's method, so a bare `title` in the
// template reads the local, but the comment's lambda doesn't put them in the body's scope and Ruby parses each read as
// a call (`self.title`). Turns those calls back into local reads: no receiver written, no arguments or parentheses.
class LocalReads {
    const vector<core::NameRef> &names;

public:
    explicit LocalReads(const vector<core::NameRef> &names) : names(names) {}

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (!send.recv.isSelfReference() || !send.recv.loc().empty() || send.numPosArgs() != 0 || send.hasKwArgs() ||
            send.hasBlock() || send.loc != send.funLoc || absl::c_find(names, send.fun) == names.end()) {
            return;
        }
        tree = ast::MK::Local(send.loc, send.fun);
    }
};

// Turns each render of a literal partial with literal locals into a call to the partial's method.
class PartialRenders {
    core::NameRef render, partial, locals;

    static optional<string_view> literalString(core::MutableContext ctx, const ast::ExpressionPtr &expr) {
        auto literal = ast::cast_tree<ast::Literal>(expr);
        if (literal == nullptr || !literal->isString()) {
            return nullopt;
        }
        return literal->asString().shortName(ctx);
    }

    // A hash literal's entries as keyword arguments, if every key is a symbol.
    static bool hashArgs(ast::ExpressionPtr &expr, ast::Send::ARGS_store &args) {
        auto hash = ast::cast_tree<ast::Hash>(expr);
        if (hash == nullptr) {
            return false;
        }
        for (auto &key : hash->keys) {
            auto literal = ast::cast_tree<ast::Literal>(key);
            if (literal == nullptr || !literal->isSymbol()) {
                return false;
            }
        }
        for (size_t i = 0; i < hash->keys.size(); i++) {
            args.emplace_back(move(hash->keys[i]));
            args.emplace_back(move(hash->values[i]));
        }
        return true;
    }

public:
    explicit PartialRenders(core::MutableContext ctx)
        : render(ctx.state.enterNameUTF8("render")), partial(ctx.state.enterNameUTF8("partial")),
          locals(ctx.state.enterNameUTF8("locals")) {}

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (send.fun != render || !send.recv.isSelfReference() || send.hasBlock() || send.hasKwSplat()) {
            return;
        }
        optional<string_view> name;
        ast::Send::ARGS_store args;
        if (send.numPosArgs() == 1 || send.numPosArgs() == 2) {
            // `render 'row', template: t`, or `render('row', { template: t })`: after a name, the hash is locals.
            name = literalString(ctx, send.getPosArg(0));
            if (!name.has_value() ||
                (send.numPosArgs() == 2 && (send.hasKwArgs() || !hashArgs(send.getPosArg(1), args)))) {
                return;
            }
            for (uint16_t i = 0; i < send.numKwArgs(); i++) {
                args.emplace_back(move(send.getKwKey(i)));
                args.emplace_back(move(send.getKwValue(i)));
            }
        } else if (send.numPosArgs() == 0 && send.hasKwArgs()) {
            // `render partial: 'row', locals: { template: t }`, and no other options.
            ast::ExpressionPtr *localsHash = nullptr;
            for (uint16_t i = 0; i < send.numKwArgs(); i++) {
                auto key = ast::cast_tree<ast::Literal>(send.getKwKey(i));
                if (key == nullptr || !key->isSymbol()) {
                    return;
                }
                if (key->asSymbol() == partial) {
                    name = literalString(ctx, send.getKwValue(i));
                } else if (key->asSymbol() == locals) {
                    localsHash = &send.getKwValue(i);
                } else {
                    return;
                }
            }
            if (!name.has_value() || (localsHash != nullptr && !hashArgs(*localsHash, args))) {
                return;
            }
        } else {
            return;
        }
        auto method = partialMethod(*name);
        if (!method.has_value()) {
            return;
        }
        tree = ast::MK::Send(send.loc, move(send.recv), ctx.state.enterNameUTF8(*method), send.funLoc, 0, move(args),
                             send.flags);
    }
};

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

        Locals locals;
        ast::InsSeq::STATS_store stats;
        for (auto &stat : classDef.rhs) {
            if (!takeLocals(ctx, stat, locals)) {
                stats.emplace_back(move(stat));
            }
        }
        classDef.rhs.clear();
        auto body = stats.empty() ? ast::MK::EmptyTree() : move(stats.back());
        if (!stats.empty()) {
            stats.pop_back();
        }
        auto code = stats.empty() ? move(body) : ast::MK::InsSeq(loc, move(stats), move(body));
        if (locals.declared) {
            vector<core::NameRef> names;
            for (auto &param : locals.params) {
                if (!ast::isa_tree<ast::BlockParam>(param)) {
                    names.emplace_back(ast::MK::arg2Name(param));
                }
            }
            LocalReads localReads(names);
            ast::TreeWalk::apply(ctx, localReads, code);
        }
        if (!found->second) {
            PartialRenders partialRenders(ctx);
            ast::TreeWalk::apply(ctx, partialRenders, code);
        }

        ast::ClassDef::RHS_store classBody;
        // What a template returns is its output, which nothing here checks: an implied `sig { void }`, so templates
        // pass at `typed: strict`.
        classBody.emplace_back(ast::MK::SigVoid(zero, sigParams(ctx, locals)));
        classBody.emplace_back(
            ast::MK::Method(loc, zero, ctx.state.enterNameUTF8("__render"), move(locals.params), move(code)));

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
