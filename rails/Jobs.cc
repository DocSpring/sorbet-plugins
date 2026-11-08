// Jobs: `perform_async`, `perform_in` and `perform_at` for Sidekiq jobs, `perform_later` and `perform_now` for
// ActiveJob jobs, from the job's `perform` and its sig. The same methods Tapioca's SidekiqWorker and ActiveJob
// compilers wrote.
//
// A job is a class in `app/jobs/` with a `perform` of its own, from an included module (followed through modules
// that include it), or from its superclass. Its kind comes from its file (`include Sidekiq::Job` or
// `Sidekiq::Worker`), else from its superclass's file by Zeitwerk path, up to four steps, stopping at a Sidekiq
// include or at `ApplicationJob` / `ActiveJob::Base`; with no answer it's a Sidekiq job.

#include "Rails.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_STEPS = 4;

enum class JobKind { Sidekiq, ActiveJob };

bool isSidekiqModule(const ConstantRef &ref) {
    auto name = join(ref.name);
    return name == "Sidekiq::Worker" || name == "Sidekiq::Job";
}

// The lexical scopes a class's superclass is looked up in: the class's own nesting without the class.
vector<ConstantName> outerNesting(const Definition &def) {
    return vector<ConstantName>(def.nesting.begin() + 1, def.nesting.end());
}

JobKind jobKind(const App &app, Definition def) {
    for (int step = 0;; step++) {
        for (auto &mixin : def.mixins("include")) {
            if (isSidekiqModule(mixin)) {
                return JobKind::Sidekiq;
            }
        }
        auto superclass = constantRef(*def.file, def.superclass);
        if (!superclass) {
            return JobKind::Sidekiq;
        }
        auto name = join(superclass->name);
        if (name == "ApplicationJob" || name == "ActiveJob::Base") {
            return JobKind::ActiveJob;
        }
        if (step + 1 >= MAX_STEPS) {
            return JobKind::Sidekiq;
        }
        auto parent = resolve(app, outerNesting(def), *superclass);
        if (!parent) {
            return JobKind::Sidekiq;
        }
        def = *parent;
    }
}

struct Perform {
    Definition owner;
    Definition::Method method;
    bool own;
};

optional<Perform> moduleProvidingPerform(const App &app, const Definition &def, int depth) {
    if (depth > MAX_STEPS) {
        return nullopt;
    }
    for (auto &mixin : def.mixins("include")) {
        auto module = resolve(app, def.nesting, mixin);
        if (!module || module->isClass) {
            continue;
        }
        if (auto method = module->method("perform")) {
            return Perform{*module, *method, false};
        }
        if (auto found = moduleProvidingPerform(app, *module, depth + 1)) {
            return found;
        }
    }
    return nullopt;
}

optional<Perform> findPerform(const App &app, const Definition &def, int depth = 0) {
    if (auto method = def.method("perform")) {
        return Perform{def, *method, depth == 0};
    }
    if (auto found = moduleProvidingPerform(app, def, 0)) {
        return found;
    }
    auto superclass = constantRef(*def.file, def.superclass);
    if (!superclass || depth + 1 >= MAX_STEPS) {
        return nullopt;
    }
    auto parent = resolve(app, outerNesting(def), *superclass);
    if (!parent) {
        return nullopt;
    }
    auto found = findPerform(app, *parent, depth + 1);
    if (found) {
        found->own = false;
    }
    return found;
}

ConstantRef absolute(const ConstantName &name) {
    return ConstantRef{name, true};
}

ast::ExpressionPtr typeConstant(core::MutableContext ctx, core::LocOffsets loc, initializer_list<string> parts) {
    return constantTree(ctx, loc, absolute(ConstantName(parts)));
}

ast::ExpressionPtr tAny(core::MutableContext ctx, core::LocOffsets loc, ast::Send::ARGS_store types) {
    auto count = static_cast<uint16_t>(types.size());
    return ast::MK::Send(loc, ast::MK::T(loc), core::Names::any(), loc, count, move(types));
}

// The three Sidekiq methods, as instance methods of a module (`singleton` false) or class methods.
void addSidekiqMethods(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc,
                       const Perform &perform, const vector<Param> &params, const Sig *sig, bool singleton) {
    for (auto name : {"perform_async", "perform_in", "perform_at"}) {
        ast::MethodDef::PARAMS_store treeParams;
        ast::Send::ARGS_store args;
        if (string_view(name) != "perform_async") {
            auto interval = ctx.state.enterNameUTF8("interval");
            treeParams.emplace_back(ast::MK::Local(loc, interval));
            args.emplace_back(ast::MK::Symbol(loc, interval));
            ast::Send::ARGS_store types;
            if (string_view(name) == "perform_in") {
                types.emplace_back(typeConstant(ctx, loc, {"Numeric"}));
                types.emplace_back(typeConstant(ctx, loc, {"ActiveSupport", "Duration"}));
            } else {
                types.emplace_back(typeConstant(ctx, loc, {"DateTime"}));
                types.emplace_back(typeConstant(ctx, loc, {"Time"}));
                types.emplace_back(typeConstant(ctx, loc, {"ActiveSupport", "TimeWithZone"}));
            }
            args.emplace_back(tAny(ctx, loc, move(types)));
        }
        for (auto &param : params) {
            treeParams.emplace_back(paramTree(ctx, loc, param));
        }
        auto copied = sigArgs(ctx, loc, *perform.owner.file, params, sig);
        for (auto &arg : *copied) {
            args.emplace_back(move(arg));
        }
        addMethod(ctx, body, loc, name, move(treeParams), move(args), typeConstant(ctx, loc, {"String"}), singleton);
    }
}

void addActiveJobMethods(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc,
                         const Perform &perform, const vector<Param> &params, const Sig *sig) {
    auto &file = *perform.owner.file;
    auto job = absolute(perform.owner.fullName);

    // perform_later: perform's parameters, with a block that takes the job in place of perform's own block.
    ast::MethodDef::PARAMS_store laterParams;
    vector<Param> withoutBlock;
    for (auto &param : params) {
        if (param.kind != Param::Kind::Block) {
            withoutBlock.emplace_back(param);
            laterParams.emplace_back(paramTree(ctx, loc, param));
        }
    }
    auto laterArgs = *sigArgs(ctx, loc, file, withoutBlock, sig);
    auto block = ctx.state.enterNameUTF8("block");
    laterParams.emplace_back(ast::MK::BlockParam(loc, ast::MK::Local(loc, block)));
    laterArgs.emplace_back(ast::MK::Symbol(loc, block));
    // T.nilable(T.proc.params(job: Job).void)
    auto proc = ast::MK::Send0(loc, ast::MK::T(loc), core::Names::proc(), loc);
    ast::Send::ARGS_store procArgs;
    procArgs.emplace_back(ast::MK::Symbol(loc, ctx.state.enterNameUTF8("job")));
    procArgs.emplace_back(constantTree(ctx, loc, job));
    proc = ast::MK::Send(loc, move(proc), core::Names::params(), loc, 0, move(procArgs));
    proc = ast::MK::Send0(loc, move(proc), core::Names::void_(), loc);
    laterArgs.emplace_back(ast::MK::Nilable(loc, move(proc)));
    ast::Send::ARGS_store laterTypes;
    laterTypes.emplace_back(constantTree(ctx, loc, job));
    laterTypes.emplace_back(typeConstant(ctx, loc, {"FalseClass"}));
    addMethod(ctx, body, loc, "perform_later", move(laterParams), move(laterArgs), tAny(ctx, loc, move(laterTypes)),
              true);

    // perform_now: perform's parameters and return type.
    ast::MethodDef::PARAMS_store nowParams;
    for (auto &param : params) {
        nowParams.emplace_back(paramTree(ctx, loc, param));
    }
    auto nowArgs = *sigArgs(ctx, loc, file, params, sig);
    optional<ast::ExpressionPtr> returns;
    if (sig == nullptr) {
        returns = ast::MK::Untyped(loc);
    } else if (sig->returns != nullptr) {
        returns = copyExpression(ctx, loc, file, sig->returns);
    }
    addMethod(ctx, body, loc, "perform_now", move(nowParams), move(nowArgs), move(returns), true);
}

// Whether every type in perform's sig can be copied.
bool copyable(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file, const Sig &sig) {
    for (auto &[name, type] : sig.params) {
        if (!copyExpression(ctx, loc, file, type)) {
            return false;
        }
    }
    return sig.returns == nullptr || copyExpression(ctx, loc, file, sig.returns).has_value();
}

} // namespace

void rewriteJobs(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    for (auto &def : definitions(file)) {
        if (!def.isClass) {
            continue;
        }
        auto perform = findPerform(app, def);
        if (!perform) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        auto loc = zeroAt(*tree);
        // The class's kind is in its own Zeitwerk file, whichever file reopens it.
        auto main = resolve(app, {}, ConstantRef{def.fullName, true});
        auto kind = jobKind(app, main ? *main : def);
        if (kind == JobKind::ActiveJob && !perform->own) {
            continue; // Tapioca's ActiveJob compiler only types a perform the job defines itself.
        }

        auto &ownerFile = *perform->owner.file;
        auto params = readParams(ownerFile, perform->method.def->parameters);
        if (!params) {
            reportError(ctx, loc,
                        fmt::format("can't copy the parameters of {}#perform ({}:{}): give each one a name",
                                    join(perform->owner.fullName), ownerFile.path,
                                    ownerFile.line(reinterpret_cast<const pm_node_t *>(perform->method.def))));
            continue;
        }
        optional<Sig> sig;
        if (perform->method.sig != nullptr) {
            sig = readSig(ownerFile, perform->method.sig);
            if (!sig || !copyable(ctx, loc, ownerFile, *sig)) {
                reportError(ctx, loc,
                            fmt::format("can't copy the sig of {}#perform ({}:{}): it has a part the plugin doesn't "
                                        "copy (generics, `bind`, a type that isn't a constant or a call)",
                                        join(perform->owner.fullName), ownerFile.path,
                                        ownerFile.line(reinterpret_cast<const pm_node_t *>(perform->method.sig))));
                continue;
            }
        }
        const Sig *sigPtr = sig ? &*sig : nullptr;

        if (perform->own) {
            if (kind == JobKind::Sidekiq) {
                addSidekiqMethods(ctx, tree->rhs, loc, *perform, *params, sigPtr, true);
            } else {
                addActiveJobMethods(ctx, tree->rhs, loc, *perform, *params, sigPtr);
            }
            continue;
        }
        // perform is in another file: the methods go in a module inside that file's nesting, so the sig's constants
        // resolve as they do there, and the job extends it.
        ast::ClassDef::RHS_store methods;
        addSidekiqMethods(ctx, methods, loc, *perform, *params, sigPtr, false);
        auto module = addUnderNesting(ctx, root, loc, perform->owner, "SorbetSidekiqMethods", move(methods));
        tree->rhs.emplace_back(mixinStatement(ctx, loc, "extend", module));
    }
}

} // namespace sorbet::plugins::rails
