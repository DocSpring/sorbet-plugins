// Uploaders: Shrine. An uploader is a class in app/ or lib/ whose superclasses up to `Shrine` are all the app's. Each
// `plugin` call in its class body mixes in the plugin's modules, as Shrine's `plugin` does: the uploader includes
// `InstanceMethods` and extends `ClassMethods`, and its own `Attacher` and `UploadedFile` subclasses (Shrine creates
// them in `Shrine.inherited`, under its superclass's) include `AttacherMethods` and `FileMethods` and extend
// `AttacherClassMethods` and `FileClassMethods`. A plugin is one of Shrine's, by name (`plugin :derivatives`, from the
// table of Shrine 3.10's below, with the plugins its `load_dependencies` loads first), or an app module (`plugin
// KeepFilesOnSoftDelete`), read from its file with the plugins its `load_dependencies` loads. A `plugin` call under an
// `if` or `unless` counts: the plugin types what the uploader may load. The app's GemMixins compiler raises for a
// `plugin` call anywhere else, which neither side would write.
//
// Attachments: `include Uploader.attachment(:name)` in a model (read in ModelMacros.cc) gets the methods that its
// uploader's plugins, its superclasses' and the global ones (`Shrine.plugin` in config/initializers/shrine.rb) define
// for an attachment, typed as the app's ShrineAttachments compiler, which this replaces, typed them: `name` (taking a
// derivative's name with the derivatives plugin, and returning a hash of versions too when a plugin's `configure` sets
// `opts[:versions]`), `name=`, `name_attacher`, `name_url`, `name_changed?`, `cached_name_data` and the derivatives
// plugin's `name_derivatives` and `name_derivatives!`. Activerecord's `reload`, and a method the model defines itself,
// are left out.

#include "Rails.h"
#include "Template.h"

#include <array>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_UPLOADER_DEPTH = 5;
constexpr int MAX_PLUGIN_DEPTH = 5;

// The modules a plugin can define, in the order Shrine's `plugin` mixes them in.
enum Kind : uint16_t {
    InstanceMethods = 1 << 0,
    ClassMethods = 1 << 1,
    FileMethods = 1 << 2,
    FileClassMethods = 1 << 3,
    AttachmentMethods = 1 << 4,
    AttachmentClassMethods = 1 << 5,
    AttacherMethods = 1 << 6,
    AttacherClassMethods = 1 << 7,
};

constexpr array<pair<Kind, string_view>, 8> KINDS = {{
    {InstanceMethods, "InstanceMethods"},
    {ClassMethods, "ClassMethods"},
    {FileMethods, "FileMethods"},
    {FileClassMethods, "FileClassMethods"},
    {AttachmentMethods, "AttachmentMethods"},
    {AttachmentClassMethods, "AttachmentClassMethods"},
    {AttacherMethods, "AttacherMethods"},
    {AttacherClassMethods, "AttacherClassMethods"},
}};

// One of Shrine's plugins: its module under `Shrine::Plugins`, the modules that defines, the attachment methods its
// AttachmentMethods define, and the plugins its `load_dependencies` loads first (`activerecord` loads `model`, which
// loads `entity`).
struct Builtin {
    string_view name;
    string_view module;
    uint16_t kinds;
    uint8_t attachment;
    array<string_view, 2> dependencies = {};
};

// An option whose `configure` includes a module of the plugin's into the uploader's Attacher when it's `true`.
struct AttacherOption {
    string_view plugin;
    string_view option;
    string_view module;
};

constexpr array<AttacherOption, 1> ATTACHER_OPTIONS = {{
    {"derivatives", "versions_compatibility", "::Shrine::Plugins::Derivatives::VersionsCompatibility"},
}};

constexpr uint8_t ENTITY = ShrineAttachment::Entity;
constexpr uint8_t MODEL = ShrineAttachment::Model;

// Shrine 3.10.2's plugins that the app loads, and those they load, from the gem's code. When Shrine is upgraded,
// check each against `lib/shrine/plugins/<name>.rb`: its modules, its `load_dependencies`, and a `configure` that
// mixes in a module (ATTACHER_OPTIONS).
constexpr array<Builtin, 27> BUILTINS = {{
    {"_persistence", "Persistence", AttacherMethods, 0, {"atomic_helpers", "entity"}},
    {"activerecord", "Activerecord", AttacherMethods | AttachmentMethods, 0, {"model", "_persistence"}},
    {"add_metadata", "AddMetadata", InstanceMethods | ClassMethods | AttacherMethods | FileMethods, 0},
    {"atomic_helpers", "AtomicHelpers", AttacherMethods | AttacherClassMethods, 0},
    {"backgrounding", "Backgrounding", AttacherMethods | AttacherClassMethods, 0},
    {"cached_attachment_data", "CachedAttachmentData", AttacherMethods | AttachmentMethods,
     ShrineAttachment::CachedData},
    {"column", "Column", AttacherMethods | AttacherClassMethods, 0},
    {"default_url", "DefaultUrl", AttacherMethods | AttacherClassMethods, 0},
    {"delete_raw", "DeleteRaw", InstanceMethods, 0},
    {"derivatives",
     "Derivatives",
     ClassMethods | AttacherMethods | AttacherClassMethods | FileMethods | AttachmentMethods,
     ShrineAttachment::Derivatives,
     {"default_url"}},
    {"determine_mime_type", "DetermineMimeType", InstanceMethods | ClassMethods, 0},
    {"entity", "Entity", AttacherMethods | AttacherClassMethods | AttachmentMethods, ENTITY, {"column"}},
    {"instrumentation", "Instrumentation", InstanceMethods | ClassMethods | FileMethods, 0},
    {"model", "Model", AttacherMethods | AttacherClassMethods | AttachmentMethods, MODEL, {"entity"}},
    {"presign_endpoint", "PresignEndpoint", ClassMethods, 0},
    {"pretty_location", "PrettyLocation", InstanceMethods, 0},
    {"rack_file", "RackFile", ClassMethods | AttacherMethods, 0},
    {"rack_response", "RackResponse", FileMethods, 0},
    {"refresh_metadata", "RefreshMetadata", AttacherMethods | FileMethods, 0},
    {"remove_invalid", "RemoveInvalid", AttacherMethods, 0, {"validation"}},
    {"restore_cached_data", "RestoreCachedData", AttacherMethods, 0, {"refresh_metadata"}},
    {"store_dimensions", "StoreDimensions", InstanceMethods | ClassMethods | FileMethods, 0},
    {"upload_endpoint", "UploadEndpoint", ClassMethods, 0, {"rack_file"}},
    {"upload_options", "UploadOptions", InstanceMethods, 0},
    {"url_options", "UrlOptions", FileMethods, 0},
    {"validation", "Validation", AttacherMethods | AttacherClassMethods, 0},
    {"validation_helpers", "ValidationHelpers", AttacherMethods | AttacherClassMethods, 0, {"validation"}},
}};

// A plugin a `plugin` call loads, or a module its `configure` includes in the Attacher.
struct Loaded {
    string module; // `::Shrine::Plugins::AddMetadata`, `::KeepFilesOnSoftDelete`
    uint16_t kinds = 0;
    uint8_t attachment = 0;
    bool versions = false;     // its `configure` sets `opts[:versions]`
    bool attacherSelf = false; // the module itself, included in the Attacher (ATTACHER_OPTIONS)
};

bool isPluginCall(string_view name) {
    return name == "plugin";
}

// A statement in a `configure` that sets `opts[:versions]` (`uploader.opts[:versions] ||= {}`).
bool setsVersions(const RubyFile &file, const pm_node_t *stat) {
    const pm_node_t *receiver = nullptr;
    const pm_arguments_node_t *arguments = nullptr;
    switch (PM_NODE_TYPE(stat)) {
        case PM_INDEX_OR_WRITE_NODE:
            receiver = reinterpret_cast<const pm_index_or_write_node_t *>(stat)->receiver;
            arguments = reinterpret_cast<const pm_index_or_write_node_t *>(stat)->arguments;
            break;
        case PM_INDEX_AND_WRITE_NODE:
            receiver = reinterpret_cast<const pm_index_and_write_node_t *>(stat)->receiver;
            arguments = reinterpret_cast<const pm_index_and_write_node_t *>(stat)->arguments;
            break;
        case PM_INDEX_OPERATOR_WRITE_NODE:
            receiver = reinterpret_cast<const pm_index_operator_write_node_t *>(stat)->receiver;
            arguments = reinterpret_cast<const pm_index_operator_write_node_t *>(stat)->arguments;
            break;
        default: {
            auto call = asCall(stat);
            if (call == nullptr || callName(file, call) != "[]=") {
                return false;
            }
            receiver = call->receiver;
            arguments = call->arguments;
        }
    }
    auto opts = asCall(receiver);
    return opts != nullptr && callName(file, opts) == "opts" && arguments != nullptr &&
           arguments->arguments.size >= 1 && nameLiteral(arguments->arguments.nodes[0]) == "versions";
}

// What Shrine's plugin `name` loads: the plugins its `load_dependencies` loads, then the plugin.
bool loadBuiltin(string_view name, int depth, vector<Loaded> &out, string &error) {
    auto builtin = absl::c_find_if(BUILTINS, [&](const Builtin &b) { return b.name == name; });
    if (builtin == BUILTINS.end()) {
        error = fmt::format("doesn't know Shrine's `{}` plugin: add it to the table in the plugin's rails/Uploaders.cc",
                            name);
        return false;
    }
    if (depth >= MAX_PLUGIN_DEPTH) {
        error = fmt::format("Shrine's `{}` plugin loads plugins more than {} deep", name, MAX_PLUGIN_DEPTH);
        return false;
    }
    for (auto dependency : builtin->dependencies) {
        if (!dependency.empty() && !loadBuiltin(dependency, depth + 1, out, error)) {
            return false;
        }
    }
    out.emplace_back(
        Loaded{fmt::format("::Shrine::Plugins::{}", builtin->module), builtin->kinds, builtin->attachment});
    return true;
}

// The module a Shrine plugin's option makes its `configure` include in the Attacher (ATTACHER_OPTIONS).
bool loadOptions(string_view name, const Arguments &args, vector<Loaded> &out, string &error) {
    for (auto &option : ATTACHER_OPTIONS) {
        if (option.plugin != name) {
            continue;
        }
        auto keyword = absl::c_find_if(args.keywords, [&](auto &pair) { return pair.first == option.option; });
        if (keyword == args.keywords.end()) {
            if (!args.splats.empty() || !args.readable) {
                error =
                    fmt::format("can't read `{}:` among the options: the plugin reads them written out", option.option);
                return false;
            }
            continue;
        }
        if (PM_NODE_TYPE_P(keyword->second, PM_TRUE_NODE)) {
            out.emplace_back(Loaded{string(option.module), 0, 0, false, true});
        } else if (!PM_NODE_TYPE_P(keyword->second, PM_FALSE_NODE)) {
            error = fmt::format("can't read `{}:`: the plugin reads `true` or `false`", option.option);
            return false;
        }
    }
    return true;
}

// A call in a method body that mixes in a module, on any receiver (`uploader::Attacher.include(X)`): the plugin can't
// follow it.
bool mixesIn(const RubyFile &file, const pm_node_t *body) {
    struct Search {
        const RubyFile *file;
        bool found = false;
    } search{&file};
    if (body == nullptr) {
        return false;
    }
    pm_visit_node(
        body,
        [](const pm_node_t *node, void *data) -> bool {
            auto search = static_cast<Search *>(data);
            auto call = asCall(node);
            if (call != nullptr) {
                auto name = callName(*search->file, call);
                search->found = search->found || name == "include" || name == "extend" || name == "prepend";
            }
            return !search->found;
        },
        &search);
    return search.found;
}

// What `call` (`plugin <arg>, options`) loads, written in `file` with lexical scopes `nesting`: the plugins the
// plugin's `load_dependencies` loads, then the plugin itself, as Shrine loads them, then what its options make its
// `configure` include. Returns false with `error` set if the plugin can't read it.
bool loadPlugin(const App &app, const RubyFile &file, const vector<ConstantName> &nesting, const pm_call_node_t *call,
                int depth, vector<Loaded> &out, string &error) {
    auto args = splitArguments(call);
    if (args.positional.empty()) {
        error = "the plugin reads a Shrine plugin given as a symbol or a constant";
        return false;
    }
    auto arg = args.positional[0];
    if (PM_NODE_TYPE_P(arg, PM_SYMBOL_NODE)) {
        auto name = nameLiteral(arg);
        return loadBuiltin(*name, depth, out, error) && loadOptions(*name, args, out, error);
    }
    auto ref = constantRef(file, arg);
    if (!ref) {
        error = "the plugin reads a Shrine plugin given as a symbol or a constant";
        return false;
    }
    auto module = resolve(app, nesting, *ref);
    if (!module || module->isClass) {
        error = fmt::format("`{}` isn't a module in the app's files", join(ref->name));
        return false;
    }
    if (depth >= MAX_PLUGIN_DEPTH) {
        error = fmt::format("`{}` loads plugins more than {} deep", join(module->fullName), MAX_PLUGIN_DEPTH);
        return false;
    }
    auto moduleName = join(module->fullName);
    if (auto dependencies = module->method("load_dependencies", true)) {
        auto params = dependencies->def->parameters;
        optional<string_view> uploader;
        if (params != nullptr && params->requireds.size >= 1 &&
            PM_NODE_TYPE_P(params->requireds.nodes[0], PM_REQUIRED_PARAMETER_NODE)) {
            uploader = module->file->name(
                reinterpret_cast<const pm_required_parameter_node_t *>(params->requireds.nodes[0])->name);
        }
        for (auto stat : statements(dependencies->def->body)) {
            auto call = asCall(stat);
            auto args = call == nullptr ? vector<const pm_node_t *>{} : arguments(call);
            auto receiver = call == nullptr ? nullptr : call->receiver;
            if (!uploader || call == nullptr || callName(*module->file, call) != "plugin" || args.empty() ||
                receiver == nullptr || !PM_NODE_TYPE_P(receiver, PM_LOCAL_VARIABLE_READ_NODE) ||
                module->file->name(reinterpret_cast<const pm_local_variable_read_node_t *>(receiver)->name) !=
                    *uploader) {
                error = fmt::format("can't read `{}.load_dependencies` (line {}): the plugin reads `uploader.plugin` "
                                    "calls",
                                    moduleName, module->file->line(stat));
                return false;
            }
            if (!loadPlugin(app, *module->file, module->nesting, call, depth + 1, out, error)) {
                return false;
            }
        }
    }
    Loaded loaded{"::" + moduleName};
    for (auto &[kind, kindName] : KINDS) {
        auto name = module->fullName;
        name.emplace_back(kindName);
        auto found = findDefinition(module->file, name);
        if (found && !found->isClass) {
            loaded.kinds |= kind;
            if (kind == AttacherMethods && found->method("get")) {
                error = fmt::format("doesn't type the attachment readers of an uploader whose plugin `{}` defines "
                                    "`Attacher#get`",
                                    moduleName);
                return false;
            }
        }
    }
    if ((loaded.kinds & (AttachmentMethods | AttachmentClassMethods)) != 0) {
        error = fmt::format("doesn't type the attachment methods that `{}` defines", moduleName);
        return false;
    }
    if (auto configure = module->method("configure", true)) {
        if (mixesIn(*module->file, configure->def->body)) {
            error = fmt::format("doesn't type the modules that `{}.configure` mixes in", moduleName);
            return false;
        }
        loaded.versions = absl::c_any_of(statements(configure->def->body),
                                         [&](const pm_node_t *stat) { return setsVersions(*module->file, stat); });
    }
    out.emplace_back(move(loaded));
    return true;
}

// The `plugin` calls (with no receiver, or `self`) in a class body's statements and in the branches of its `if` and
// `unless` statements; `unreadable` gets those anywhere else (a block, a method call's arguments).
void pluginCalls(const RubyFile &file, const vector<const pm_node_t *> &stats, bool onShrine,
                 vector<const pm_call_node_t *> &calls, vector<const pm_call_node_t *> &unreadable) {
    for (auto stat : stats) {
        if (PM_NODE_TYPE_P(stat, PM_IF_NODE)) {
            auto node = reinterpret_cast<const pm_if_node_t *>(stat);
            pluginCalls(file, statements(reinterpret_cast<const pm_node_t *>(node->statements)), onShrine, calls,
                        unreadable);
            if (node->subsequent != nullptr) {
                pluginCalls(file, {node->subsequent}, onShrine, calls, unreadable);
            }
            continue;
        }
        if (PM_NODE_TYPE_P(stat, PM_ELSE_NODE)) {
            pluginCalls(file,
                        statements(reinterpret_cast<const pm_node_t *>(
                            reinterpret_cast<const pm_else_node_t *>(stat)->statements)),
                        onShrine, calls, unreadable);
            continue;
        }
        if (PM_NODE_TYPE_P(stat, PM_UNLESS_NODE)) {
            auto node = reinterpret_cast<const pm_unless_node_t *>(stat);
            pluginCalls(file, statements(reinterpret_cast<const pm_node_t *>(node->statements)), onShrine, calls,
                        unreadable);
            if (node->else_clause != nullptr) {
                pluginCalls(file, {reinterpret_cast<const pm_node_t *>(node->else_clause)}, onShrine, calls,
                            unreadable);
            }
            continue;
        }
        auto call = asCall(stat);
        if (call != nullptr && callName(file, call) == "plugin") {
            if (onShrine) {
                auto receiver = call->receiver == nullptr ? nullopt : constantRef(file, call->receiver);
                if (receiver && join(receiver->name) == "Shrine") {
                    calls.emplace_back(call);
                    continue;
                }
            } else if (call->receiver == nullptr || isSelf(call->receiver)) {
                calls.emplace_back(call);
                continue;
            }
        }
        if (onShrine) {
            continue;
        }
        for (auto nested : macroCalls(file, stat, isPluginCall)) {
            unreadable.emplace_back(nested);
        }
    }
}

// The uploader and its superclasses up to the one directly under Shrine, if every one is an app class.
optional<vector<Definition>> uploaderChain(const App &app, const Definition &def) {
    vector<Definition> chain{def};
    for (int depth = 0; depth < MAX_UPLOADER_DEPTH; depth++) {
        auto &current = chain.back();
        if (!current.isClass || current.superclass == nullptr) {
            return nullopt;
        }
        auto superclass = constantRef(*current.file, current.superclass);
        if (!superclass) {
            return nullopt;
        }
        if (join(superclass->name) == "Shrine") {
            return chain;
        }
        auto parent = resolve(app, current.outerNesting(), *superclass);
        if (!parent || !parent->isClass) {
            return nullopt;
        }
        chain.emplace_back(move(*parent));
    }
    return nullopt;
}

// The plugins one uploader class body loads, or nullopt with `error` set (and `node` at the call).
optional<vector<Loaded>> classPlugins(const App &app, const Definition &def, string &error, const pm_node_t *&node) {
    vector<const pm_call_node_t *> calls;
    vector<const pm_call_node_t *> unreadable;
    pluginCalls(*def.file, def.body, false, calls, unreadable);
    if (!unreadable.empty()) {
        node = reinterpret_cast<const pm_node_t *>(unreadable.front());
        error = "can't read `plugin` here: the plugin reads it directly in an uploader's class body, or in an `if` "
                "or `unless` there";
        return nullopt;
    }
    vector<Loaded> loaded;
    for (auto call : calls) {
        string problem;
        if (!loadPlugin(app, *def.file, def.nesting, call, 0, loaded, problem)) {
            node = reinterpret_cast<const pm_node_t *>(call);
            error = fmt::format("can't read this `plugin`: {}", problem);
            return nullopt;
        }
    }
    return loaded;
}

// The attachment methods of the plugins config/initializers/shrine.rb loads on Shrine (`Shrine.plugin :activerecord`).
optional<uint8_t> globalAttachment(const App &app, string &error) {
    auto path = app.find("config/initializers/shrine.rb");
    if (!path) {
        return 0;
    }
    auto file = RubyFile::read(*path);
    if (!file) {
        return 0;
    }
    vector<const pm_call_node_t *> calls;
    vector<const pm_call_node_t *> unreadable;
    pluginCalls(*file, file->topLevel(), true, calls, unreadable);
    uint8_t attachment = 0;
    for (auto call : calls) {
        vector<Loaded> loaded;
        string problem;
        if (!loadPlugin(app, *file, {}, call, 0, loaded, problem)) {
            error = fmt::format("can't read `Shrine.plugin` in config/initializers/shrine.rb (line {}): {}",
                                file->line(reinterpret_cast<const pm_node_t *>(call)), problem);
            return nullopt;
        }
        for (auto &plugin : loaded) {
            attachment |= plugin.attachment;
        }
    }
    return attachment;
}

} // namespace

optional<UploaderPlugins> uploaderPlugins(const App &app, const Definition &def, string &error) {
    auto chain = uploaderChain(app, def);
    if (!chain) {
        error = fmt::format("`{}` isn't an uploader: a class whose superclasses up to Shrine are the app's",
                            join(def.fullName));
        return nullopt;
    }
    auto global = globalAttachment(app, error);
    if (!global) {
        return nullopt;
    }
    UploaderPlugins out{def.fullName, *global};
    for (auto &level : *chain) {
        const pm_node_t *node = nullptr;
        string problem;
        auto loaded = classPlugins(app, level, problem, node);
        if (!loaded) {
            auto path = app.relative(level.file->path).value_or(level.file->path);
            error = fmt::format("{} line {}: {}", path, level.file->line(node), problem);
            return nullopt;
        }
        for (auto &plugin : *loaded) {
            out.attachment |= plugin.attachment;
            out.versions = out.versions || plugin.versions;
        }
    }
    return out;
}

string attachmentMethods(const UploaderPlugins &plugins, string_view name, const function<bool(string_view)> &skip) {
    auto uploader = "::" + join(plugins.uploader);
    auto file = fmt::format("{}::UploadedFile", uploader);
    auto fileType = plugins.versions ? fmt::format("T.any({0}, T::Hash[::Symbol, {0}])", file) : file;
    auto derivatives = (plugins.attachment & ShrineAttachment::Derivatives) != 0;
    string out;
    auto add = [&](const string &method, const string &text) {
        if (!skip(method)) {
            out += text;
        }
    };
    if ((plugins.attachment & ShrineAttachment::Entity) != 0) {
        if (derivatives) {
            add(string(name), fmt::format("sig {{ params(args: T.untyped).returns(T.nilable({1})) }}\n"
                                          "def {0}(*args); end\n\n",
                                          name, fileType));
        } else {
            add(string(name), fmt::format("sig {{ returns(T.nilable({1})) }}\ndef {0}; end\n\n", name, fileType));
        }
        add(fmt::format("{}_attacher", name),
            fmt::format("sig {{ params(options: T.untyped).returns({1}::Attacher) }}\n"
                        "def {0}_attacher(**options); end\n\n",
                        name, uploader));
        add(fmt::format("{}_url", name),
            fmt::format("sig {{ params(args: T.untyped, options: T.untyped).returns(T.nilable(::String)) }}\n"
                        "def {0}_url(*args, **options); end\n\n",
                        name));
    }
    if ((plugins.attachment & ShrineAttachment::Model) != 0) {
        add(fmt::format("{}=", name),
            fmt::format("sig {{ params(value: T.untyped).returns(T.untyped) }}\ndef {0}=(value); end\n\n", name));
        add(fmt::format("{}_changed?", name),
            fmt::format("sig {{ returns(T::Boolean) }}\ndef {0}_changed?; end\n\n", name));
    }
    if ((plugins.attachment & ShrineAttachment::CachedData) != 0) {
        add(fmt::format("cached_{}_data", name),
            fmt::format("sig {{ returns(T.nilable(::String)) }}\ndef cached_{0}_data; end\n\n", name));
    }
    if (derivatives) {
        for (auto suffix : {"", "!"}) {
            add(fmt::format("{}_derivatives{}", name, suffix),
                fmt::format("sig {{ params(args: T.untyped, options: T.untyped, block: T.untyped).returns(T.untyped) "
                            "}}\ndef {0}_derivatives{1}(*args, **options, &block); end\n\n",
                            name, suffix));
        }
    }
    return out;
}

void rewriteUploaders(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    for (auto &def : definitions(file)) {
        if (!def.isClass || def.superclass == nullptr) {
            continue;
        }
        // Only in the file Zeitwerk loads the class from: a file that reopens it adds nothing.
        auto main = resolve(app, {}, ConstantRef{def.fullName, true});
        if (!main || main->file->path != file->path) {
            continue;
        }
        auto chain = uploaderChain(app, def);
        if (!chain) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        auto loc = zeroAt(*tree);
        string error;
        const pm_node_t *node = nullptr;
        auto loaded = classPlugins(app, def, error, node);
        if (!loaded) {
            auto [begin, end] = file->offsets(node);
            reportError(ctx, core::LocOffsets{begin, end}, error);
            continue;
        }
        auto parent = chain->size() > 1 ? "::" + join((*chain)[1].fullName) : string("::Shrine");
        // Each scope's mixins, in the order Shrine's `plugin` mixes them in.
        string uploader;
        string fileClass;
        string attacher;
        for (auto &plugin : *loaded) {
            if (plugin.attacherSelf) {
                attacher += fmt::format("include {}\n", plugin.module);
                continue;
            }
            for (auto &[kind, kindName] : KINDS) {
                if ((plugin.kinds & kind) == 0) {
                    continue;
                }
                auto line = fmt::format(
                    "{} {}::{}\n",
                    (kind == ClassMethods || kind == FileClassMethods || kind == AttacherClassMethods) ? "extend"
                                                                                                       : "include",
                    plugin.module, kindName);
                if (kind == InstanceMethods || kind == ClassMethods) {
                    uploader += line;
                } else if (kind == FileMethods || kind == FileClassMethods) {
                    fileClass += line;
                } else if (kind == AttacherMethods || kind == AttacherClassMethods) {
                    attacher += line;
                }
            }
        }
        auto source = fmt::format("{0}\nclass Attacher < {1}::Attacher\n{2}end\n\n"
                                  "class UploadedFile < {1}::UploadedFile\n{3}end\n",
                                  uploader, parent, attacher, fileClass);
        addTemplate(ctx, loc, tree->rhs, move(source), "uploader");
    }
}

} // namespace sorbet::plugins::rails
