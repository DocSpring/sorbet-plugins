#include "Source.h"
#include "Inflector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>

using namespace std;

namespace sorbet::plugins::rails {

string join(const ConstantName &name) {
    string out;
    for (auto &part : name) {
        if (!out.empty()) {
            out += "::";
        }
        out += part;
    }
    return out;
}

namespace {

bool exists(const string &path) {
    error_code ec;
    return filesystem::exists(path, ec);
}

// The app root from a path: everything before its last `app/` or `lib/` directory.
optional<string> rootBefore(string_view path, string_view dir) {
    string slashed = "/" + string(dir);
    auto at = path.rfind(slashed);
    if (at != string_view::npos) {
        return string(path.substr(0, at + 1));
    }
    if (path.substr(0, dir.size()) == dir) {
        return string();
    }
    return nullopt;
}

} // namespace

string normalizedName(string_view name) {
    string out;
    for (auto c : name) {
        if (c != '_') {
            out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

optional<App> App::forPath(string_view path) {
    if (auto root = rootBefore(path, "app/")) {
        return App{*root};
    }
    if (auto root = rootBefore(path, "lib/")) {
        return App{*root};
    }
    return nullopt;
}

optional<string> App::find(string_view relative) const {
    auto under = root + string(relative);
    if (exists(under)) {
        return under;
    }
    if (!root.empty() && root != "./" && exists(string(relative))) {
        return string(relative);
    }
    return nullopt;
}

namespace {

// Directory listings, kept for a second. Every class the plugin types looks up constants in every autoload
// directory, and listing them each time made `srb tc` take half as long again. A second is short enough that the
// language server sees a new file almost at once; file contents are never kept.
using Names = shared_ptr<const vector<string>>;
struct Listing {
    chrono::steady_clock::time_point read;
    Names names;
};
shared_mutex listingsMutex;
unordered_map<string, Listing> listings;
constexpr auto LISTING_LIFETIME = chrono::seconds(1);

// Sorted.
Names listDirectory(const string &dir) {
    auto now = chrono::steady_clock::now();
    {
        shared_lock<shared_mutex> lock(listingsMutex);
        auto it = listings.find(dir);
        if (it != listings.end() && now - it->second.read < LISTING_LIFETIME) {
            return it->second.names;
        }
    }
    vector<string> names;
    error_code ec;
    for (filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names.emplace_back(it->path().filename().string());
    }
    sort(names.begin(), names.end());
    auto shared = make_shared<const vector<string>>(move(names));
    unique_lock<shared_mutex> lock(listingsMutex);
    listings[dir] = Listing{now, shared};
    return shared;
}

} // namespace

vector<string> App::list(string_view relativeDir) const {
    auto names = listDirectory(root + string(relativeDir));
    if (root.empty() || root == "./") {
        return *names;
    }
    vector<string> merged = *names;
    // Held in a local: a range-for doesn't keep a temporary alive, and another thread may replace the cached
    // listing (and free it) while this one reads it.
    auto inWorkingDirectory = listDirectory(string(relativeDir));
    for (auto &name : *inWorkingDirectory) {
        if (std::find(merged.begin(), merged.end(), name) == merged.end()) {
            merged.emplace_back(name);
        }
    }
    sort(merged.begin(), merged.end());
    return merged;
}

bool App::isDirectory(string_view relative) const {
    auto path = find(relative);
    error_code ec;
    return path.has_value() && filesystem::is_directory(*path, ec);
}

optional<string> App::relative(string_view path) const {
    if (path.substr(0, root.size()) != root) {
        return nullopt;
    }
    auto rest = path.substr(root.size());
    if (rest.substr(0, 2) == "./") {
        rest = rest.substr(2);
    }
    return string(rest);
}

RubyFile::RubyFile(string path, string source_) : path(move(path)), source(move(source_)) {
    pm_parser_init(&parser, reinterpret_cast<const uint8_t *>(source.data()), source.size(), nullptr);
    root_ = pm_parse(&parser);
}

shared_ptr<RubyFile> RubyFile::read(const string &path) {
    ifstream in(path, ios::binary);
    if (!in) {
        return nullptr;
    }
    stringstream buffer;
    buffer << in.rdbuf();
    return make_shared<RubyFile>(path, buffer.str());
}

RubyFile::~RubyFile() {
    pm_node_destroy(&parser, root_);
    pm_parser_free(&parser);
}

string_view RubyFile::name(pm_constant_id_t id) const {
    // Prism leaves a name it couldn't parse unset (`Foo::` with nothing after it, which the language server sees while
    // someone types), and the constant pool has no entry for it.
    if (id == PM_CONSTANT_ID_UNSET) {
        return {};
    }
    auto constant = pm_constant_pool_id_to_constant(&parser.constant_pool, id);
    return string_view(reinterpret_cast<const char *>(constant->start), constant->length);
}

string_view RubyFile::slice(const pm_location_t &loc) const {
    return string_view(reinterpret_cast<const char *>(loc.start), loc.end - loc.start);
}

int RubyFile::line(const pm_node_t *node) const {
    auto offset = reinterpret_cast<const char *>(node->location.start) - source.data();
    return 1 + static_cast<int>(count(source.begin(), source.begin() + offset, '\n'));
}

pair<uint32_t, uint32_t> RubyFile::offsets(const pm_node_t *node) const {
    auto begin = reinterpret_cast<const char *>(node->location.start) - source.data();
    auto end = reinterpret_cast<const char *>(node->location.end) - source.data();
    return {static_cast<uint32_t>(begin), static_cast<uint32_t>(end)};
}

optional<ConstantRef> constantRef(const RubyFile &file, const pm_node_t *node) {
    if (node == nullptr) {
        return nullopt;
    }
    if (PM_NODE_TYPE_P(node, PM_CONSTANT_READ_NODE)) {
        return ConstantRef{{string(file.name(reinterpret_cast<const pm_constant_read_node_t *>(node)->name))}, false};
    }
    if (PM_NODE_TYPE_P(node, PM_CONSTANT_PATH_NODE)) {
        auto path = reinterpret_cast<const pm_constant_path_node_t *>(node);
        ConstantRef ref;
        if (path->parent == nullptr) {
            ref.absolute = true;
        } else {
            auto parent = constantRef(file, path->parent);
            if (!parent) {
                return nullopt;
            }
            ref = *parent;
        }
        // `Foo::` with nothing after it: not a constant (yet).
        if (path->name == PM_CONSTANT_ID_UNSET) {
            return nullopt;
        }
        ref.name.emplace_back(file.name(path->name));
        return ref;
    }
    return nullopt;
}

const pm_call_node_t *asCall(const pm_node_t *node) {
    return node != nullptr && PM_NODE_TYPE_P(node, PM_CALL_NODE) ? reinterpret_cast<const pm_call_node_t *>(node)
                                                                 : nullptr;
}

bool isSelf(const pm_node_t *node) {
    return node != nullptr && PM_NODE_TYPE_P(node, PM_SELF_NODE);
}

string_view callName(const RubyFile &file, const pm_call_node_t *call) {
    return file.name(call->name);
}

vector<const pm_node_t *> arguments(const pm_call_node_t *call) {
    vector<const pm_node_t *> out;
    if (call->arguments != nullptr) {
        for (size_t i = 0; i < call->arguments->arguments.size; i++) {
            out.emplace_back(call->arguments->arguments.nodes[i]);
        }
    }
    return out;
}

vector<const pm_node_t *> statements(const pm_node_t *body) {
    vector<const pm_node_t *> out;
    if (body == nullptr) {
        return out;
    }
    if (PM_NODE_TYPE_P(body, PM_BEGIN_NODE)) {
        body = reinterpret_cast<const pm_node_t *>(reinterpret_cast<const pm_begin_node_t *>(body)->statements);
        if (body == nullptr) {
            return out;
        }
    }
    if (PM_NODE_TYPE_P(body, PM_STATEMENTS_NODE)) {
        auto list = reinterpret_cast<const pm_statements_node_t *>(body)->body;
        for (size_t i = 0; i < list.size; i++) {
            out.emplace_back(list.nodes[i]);
        }
    } else {
        out.emplace_back(body);
    }
    return out;
}

string stringValue(const pm_string_t &value) {
    return string(reinterpret_cast<const char *>(pm_string_source(&value)), pm_string_length(&value));
}

vector<ConstantRef> Definition::mixins(string_view methodName) const {
    vector<ConstantRef> out;
    for (auto stat : body) {
        if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
            continue;
        }
        auto call = reinterpret_cast<const pm_call_node_t *>(stat);
        if (call->receiver != nullptr || callName(*file, call) != methodName) {
            continue;
        }
        for (auto arg : arguments(call)) {
            if (auto ref = constantRef(*file, arg)) {
                out.emplace_back(*ref);
            }
        }
    }
    return out;
}

bool Definition::isConcern() const {
    if (isClass) {
        return false;
    }
    for (auto &ref : mixins("extend")) {
        if (join(ref.name) == "ActiveSupport::Concern") {
            return true;
        }
    }
    return false;
}

vector<const pm_block_node_t *> Definition::includedBlocks() const {
    vector<const pm_block_node_t *> out;
    for (auto stat : body) {
        if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
            continue;
        }
        auto call = reinterpret_cast<const pm_call_node_t *>(stat);
        if (call->receiver == nullptr && callName(*file, call) == "included" && call->block != nullptr &&
            PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
            out.emplace_back(reinterpret_cast<const pm_block_node_t *>(call->block));
        }
    }
    return out;
}

namespace {

bool isSig(const RubyFile &file, const pm_node_t *stat) {
    if (stat == nullptr || !PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
        return false;
    }
    auto call = reinterpret_cast<const pm_call_node_t *>(stat);
    return callName(file, call) == "sig" && call->block != nullptr && PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE);
}

optional<Definition::Method> findMethodIn(const RubyFile &file, const vector<const pm_node_t *> &body, string_view name,
                                          bool singleton) {
    for (size_t i = 0; i < body.size(); i++) {
        auto stat = body[i];
        if (singleton && PM_NODE_TYPE_P(stat, PM_SINGLETON_CLASS_NODE)) {
            auto sclass = reinterpret_cast<const pm_singleton_class_node_t *>(stat);
            if (sclass->expression != nullptr && PM_NODE_TYPE_P(sclass->expression, PM_SELF_NODE)) {
                if (auto found = findMethodIn(file, statements(sclass->body), name, false)) {
                    return found;
                }
            }
            continue;
        }
        // A `def` passed to a method, as `private def foo` and `helper_method def foo` are, defines it too.
        auto wrapper = asCall(stat);
        if (wrapper != nullptr && wrapper->receiver == nullptr) {
            auto args = arguments(wrapper);
            if (args.size() == 1 && PM_NODE_TYPE_P(args[0], PM_DEF_NODE)) {
                stat = args[0];
            }
        }
        if (!PM_NODE_TYPE_P(stat, PM_DEF_NODE)) {
            continue;
        }
        auto def = reinterpret_cast<const pm_def_node_t *>(stat);
        auto isSelf = def->receiver != nullptr && PM_NODE_TYPE_P(def->receiver, PM_SELF_NODE);
        if (file.name(def->name) != name || isSelf != singleton || (def->receiver != nullptr && !isSelf)) {
            continue;
        }
        const pm_call_node_t *sig = nullptr;
        if (i > 0 && isSig(file, body[i - 1])) {
            sig = reinterpret_cast<const pm_call_node_t *>(body[i - 1]);
        }
        return Definition::Method{def, sig};
    }
    return nullopt;
}

void collect(const shared_ptr<RubyFile> &file, const vector<const pm_node_t *> &stats, const Definition *outer,
             vector<Definition> &out) {
    for (auto stat : stats) {
        auto isClass = PM_NODE_TYPE_P(stat, PM_CLASS_NODE);
        if (!isClass && !PM_NODE_TYPE_P(stat, PM_MODULE_NODE)) {
            continue;
        }
        const pm_node_t *constantPath;
        const pm_node_t *bodyNode;
        const pm_node_t *superclass = nullptr;
        if (isClass) {
            auto node = reinterpret_cast<const pm_class_node_t *>(stat);
            constantPath = node->constant_path;
            bodyNode = node->body;
            superclass = node->superclass;
        } else {
            auto node = reinterpret_cast<const pm_module_node_t *>(stat);
            constantPath = node->constant_path;
            bodyNode = node->body;
        }
        auto written = constantRef(*file, constantPath);
        if (!written) {
            continue;
        }
        Definition def{file, stat, isClass, {}, {}, {}, *written, superclass, statements(bodyNode)};
        if (written->absolute || outer == nullptr) {
            def.fullName = written->name;
        } else {
            def.fullName = outer->fullName;
            def.fullName.insert(def.fullName.end(), written->name.begin(), written->name.end());
        }
        def.nesting.emplace_back(def.fullName);
        if (outer != nullptr) {
            def.nesting.insert(def.nesting.end(), outer->nesting.begin(), outer->nesting.end());
            def.enclosing = outer->enclosing;
            def.enclosing.emplace_back(outer->isClass, outer->written);
        }
        out.emplace_back(def);
        auto body = out.back().body;
        auto copy = out.back();
        collect(file, body, &copy, out);
    }
}

// The file Zeitwerk loads `name` from, if there's one.
optional<string> constantFile(const App &app, const ConstantName &name) {
    vector<string> roots;
    for (auto &dir : app.list("app")) {
        if (dir == "assets" || dir == "javascript" || dir == "views" || dir.find('.') != string::npos) {
            continue;
        }
        roots.emplace_back("app/" + dir + "/");
        roots.emplace_back("app/" + dir + "/concerns/");
    }
    roots.emplace_back("lib/");
    // A directory with a namespace of its own (`app/billing/` holds `Billing::...`).
    roots.emplace_back("app/");
    for (auto &root : roots) {
        string path = root;
        bool found = true;
        for (size_t i = 0; i < name.size() && found; i++) {
            auto last = i + 1 == name.size();
            auto want = normalizedName(name[i]);
            vector<pair<string, string>> matches; // (entry, stem)
            for (auto &entry : app.list(path)) {
                auto stem = last ? (entry.size() > 3 && entry.substr(entry.size() - 3) == ".rb"
                                        ? entry.substr(0, entry.size() - 3)
                                        : string())
                                 : entry;
                if (!stem.empty() && entry.find('.') == (last ? entry.size() - 3 : string::npos) &&
                    normalizedName(stem) == want) {
                    matches.emplace_back(entry, stem);
                }
            }
            if (matches.size() > 1) {
                // Names that differ only in underscores (`a_bc.rb`, `ab_c.rb`): Zeitwerk loads the constant from the
                // one the app's inflector camelizes to it (`ABc`, `AbC`).
                string error;
                auto inflector = Inflector::forApp(app, error);
                erase_if(matches,
                         [&](auto &match) { return !error.empty() || inflector.camelize(match.second) != name[i]; });
            }
            found = matches.size() == 1;
            if (found) {
                path += last ? matches[0].first : matches[0].first + "/";
            }
        }
        if (found) {
            if (auto existing = app.find(path)) {
                return existing;
            }
        }
    }
    return nullopt;
}

} // namespace

optional<Definition::Method> Definition::method(string_view name, bool singleton) const {
    return findMethodIn(*file, body, name, singleton);
}

vector<pair<string, Definition::Method>> Definition::publicMethods() const {
    enum class Visibility { Public, Private, Protected };
    auto visibilityOf = [](string_view name) -> optional<Visibility> {
        if (name == "public") {
            return Visibility::Public;
        }
        if (name == "private") {
            return Visibility::Private;
        }
        if (name == "protected") {
            return Visibility::Protected;
        }
        return nullopt;
    };
    vector<string> order;
    unordered_map<string, pair<Visibility, Method>> methods;
    auto define = [&](const pm_def_node_t *def, const pm_call_node_t *sig, Visibility visibility) {
        auto name = string(file->name(def->name));
        if (!methods.contains(name)) {
            order.emplace_back(name);
        }
        methods.insert_or_assign(name, pair{visibility, Method{def, sig}});
    };
    auto current = Visibility::Public;
    for (size_t i = 0; i < body.size(); i++) {
        auto stat = body[i];
        auto sig = i > 0 && isSig(*file, body[i - 1]) ? reinterpret_cast<const pm_call_node_t *>(body[i - 1]) : nullptr;
        if (PM_NODE_TYPE_P(stat, PM_DEF_NODE)) {
            auto def = reinterpret_cast<const pm_def_node_t *>(stat);
            if (def->receiver == nullptr) {
                define(def, sig, current);
            }
            continue;
        }
        if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
            continue;
        }
        auto call = reinterpret_cast<const pm_call_node_t *>(stat);
        auto visibility = call->receiver == nullptr ? visibilityOf(callName(*file, call)) : nullopt;
        if (!visibility) {
            continue;
        }
        auto args = arguments(call);
        if (args.empty()) {
            current = *visibility;
            continue;
        }
        for (auto arg : args) {
            if (PM_NODE_TYPE_P(arg, PM_DEF_NODE)) {
                auto def = reinterpret_cast<const pm_def_node_t *>(arg);
                if (def->receiver == nullptr) {
                    define(def, sig, *visibility);
                }
            } else if (PM_NODE_TYPE_P(arg, PM_SYMBOL_NODE)) {
                auto name = stringValue(reinterpret_cast<const pm_symbol_node_t *>(arg)->unescaped);
                if (auto found = methods.find(name); found != methods.end()) {
                    found->second.first = *visibility;
                }
            }
        }
    }
    vector<pair<string, Method>> out;
    for (auto &name : order) {
        auto &[visibility, method] = methods.at(name);
        if (visibility == Visibility::Public) {
            out.emplace_back(name, method);
        }
    }
    return out;
}

vector<const pm_node_t *> RubyFile::topLevel() const {
    auto program = reinterpret_cast<const pm_program_node_t *>(root_);
    return statements(reinterpret_cast<const pm_node_t *>(program->statements));
}

vector<Definition> definitions(const shared_ptr<RubyFile> &file) {
    vector<Definition> out;
    collect(file, file->topLevel(), nullptr, out);
    return out;
}

optional<Definition> findDefinition(const shared_ptr<RubyFile> &file, const ConstantName &fullName) {
    for (auto &def : definitions(file)) {
        if (def.fullName == fullName) {
            return def;
        }
    }
    return nullopt;
}

optional<Definition> resolve(const App &app, const vector<ConstantName> &nesting, const ConstantRef &ref) {
    vector<ConstantName> candidates;
    if (!ref.absolute) {
        for (auto &scope : nesting) {
            auto candidate = scope;
            candidate.insert(candidate.end(), ref.name.begin(), ref.name.end());
            candidates.emplace_back(candidate);
        }
    }
    candidates.emplace_back(ref.name);
    for (auto &candidate : candidates) {
        auto path = constantFile(app, candidate);
        if (!path) {
            continue;
        }
        auto file = RubyFile::read(*path);
        if (file == nullptr) {
            continue;
        }
        if (auto def = findDefinition(file, candidate)) {
            return def;
        }
    }
    return nullopt;
}

optional<string> nameLiteral(const pm_node_t *node) {
    if (node != nullptr && PM_NODE_TYPE_P(node, PM_SYMBOL_NODE)) {
        return stringValue(reinterpret_cast<const pm_symbol_node_t *>(node)->unescaped);
    }
    if (node != nullptr && PM_NODE_TYPE_P(node, PM_STRING_NODE)) {
        return stringValue(reinterpret_cast<const pm_string_node_t *>(node)->unescaped);
    }
    return nullopt;
}

namespace {

void addPairs(const pm_node_list_t &elements, Arguments &out) {
    for (size_t i = 0; i < elements.size; i++) {
        auto element = elements.nodes[i];
        if (PM_NODE_TYPE_P(element, PM_ASSOC_SPLAT_NODE)) {
            out.splats.emplace_back(reinterpret_cast<const pm_assoc_splat_node_t *>(element));
            continue;
        }
        auto assoc = reinterpret_cast<const pm_assoc_node_t *>(element);
        auto key = PM_NODE_TYPE_P(element, PM_ASSOC_NODE) && PM_NODE_TYPE_P(assoc->key, PM_SYMBOL_NODE)
                       ? nameLiteral(assoc->key)
                       : nullopt;
        if (!key) {
            out.readable = false;
            continue;
        }
        out.keywords.emplace_back(*key, assoc->value);
    }
}

} // namespace

Arguments splitArguments(const pm_call_node_t *call) {
    Arguments out;
    for (auto arg : arguments(call)) {
        if (!PM_NODE_TYPE_P(arg, PM_KEYWORD_HASH_NODE)) {
            out.positional.emplace_back(arg);
            continue;
        }
        addPairs(reinterpret_cast<const pm_keyword_hash_node_t *>(arg)->elements, out);
    }
    return out;
}

Arguments hashArguments(const pm_hash_node_t *hash) {
    Arguments out;
    addPairs(hash->elements, out);
    return out;
}

vector<const pm_call_node_t *> macroCalls(const RubyFile &file, const pm_node_t *node,
                                          const function<bool(string_view)> &isMacro, bool localReceivers) {
    struct Search {
        const RubyFile *file;
        const function<bool(string_view)> *isMacro;
        bool localReceivers;
        vector<const pm_call_node_t *> found;
    } search{&file, &isMacro, localReceivers, {}};
    if (node == nullptr) {
        return {};
    }
    pm_visit_node(
        node,
        [](const pm_node_t *node, void *data) -> bool {
            auto search = static_cast<Search *>(data);
            switch (PM_NODE_TYPE(node)) {
                case PM_DEF_NODE:
                case PM_CLASS_NODE:
                case PM_MODULE_NODE:
                case PM_SINGLETON_CLASS_NODE:
                case PM_LAMBDA_NODE:
                    return false;
                default:
                    break;
            }
            auto call = asCall(node);
            auto receiverOk = call != nullptr &&
                              (call->receiver == nullptr || isSelf(call->receiver) ||
                               (search->localReceivers && PM_NODE_TYPE_P(call->receiver, PM_LOCAL_VARIABLE_READ_NODE)));
            if (receiverOk && (*search->isMacro)(callName(*search->file, call))) {
                search->found.emplace_back(call);
            }
            return true;
        },
        &search);
    return search.found;
}

} // namespace sorbet::plugins::rails
