// Jobs: `perform_async`, `perform_in` and `perform_at` for Sidekiq jobs, `perform_later` and `perform_now` for
// ActiveJob jobs, from the job's `perform` and its sig. The same methods Tapioca's SidekiqWorker and ActiveJob
// compilers wrote.
//
// A job is a class in `app/jobs/` with a `perform` of its own, from an included module (followed through modules
// that include it), or from its superclass. Its kind comes from its file (`include Sidekiq::Job` or
// `Sidekiq::Worker`), else from its superclass's file by Zeitwerk path, up to four steps, stopping at a Sidekiq
// include or at `ApplicationJob` / `ActiveJob::Base`; with no answer it's a Sidekiq job.

#include "Rails.h"
#include "Template.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_STEPS = 4;

enum class JobKind { Sidekiq, ActiveJob };

bool isSidekiqModule(const ConstantRef &ref) {
    auto name = join(ref.name);
    return name == "Sidekiq::Worker" || name == "Sidekiq::Job";
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
        auto parent = resolve(app, def.outerNesting(), *superclass);
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
    auto parent = resolve(app, def.outerNesting(), *superclass);
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

// The three Sidekiq methods, as class methods, with the types from `perform`'s sig, or (`aliases`) the type aliases
// addTypeAliases wrote for them.
void addSidekiqMethods(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc,
                       const Perform &perform, const vector<Param> &params, const Sig *sig,
                       const optional<ConstantName> &aliases) {
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
        if (aliases) {
            for (size_t index = 0; index < params.size(); index++) {
                args.emplace_back(ast::MK::Symbol(loc, ctx.state.enterNameUTF8(params[index].name)));
                auto alias = *aliases;
                alias.emplace_back(fmt::format("Param{}", index));
                args.emplace_back(constantTree(ctx, loc, absolute(alias)));
            }
        } else {
            auto copied = sigArgs(ctx, loc, *perform.owner.file, params, sig);
            for (auto &arg : *copied) {
                args.emplace_back(move(arg));
            }
        }
        addMethod(ctx, body, loc, name, move(treeParams), move(args), typeConstant(ctx, loc, {"String"}), true);
    }
}

// `perform`'s parameter types as `Param<n> = T.type_alias { <type> }` in a module named `name` inside the nesting of
// the file `perform` is in, so their constants resolve as they do there. Returns the module's full name.
optional<ConstantName> addTypeAliases(core::MutableContext ctx, ast::ClassDef &root, core::LocOffsets loc,
                                      const Perform &perform, const vector<Param> &params, const Sig *sig,
                                      string_view name) {
    auto args = sigArgs(ctx, loc, *perform.owner.file, params, sig);
    if (!args) {
        return nullopt;
    }
    ast::ClassDef::RHS_store body;
    for (size_t index = 0; index < params.size(); index++) {
        auto constant = ast::MK::UnresolvedConstant(loc, ast::MK::EmptyTree(),
                                                    ctx.state.enterNameConstant(fmt::format("Param{}", index)));
        auto alias = ast::MK::Send0Block(loc, ast::MK::T(loc), core::Names::typeAlias(), loc,
                                         ast::MK::Block0(loc, move((*args)[index * 2 + 1])));
        body.emplace_back(ast::MK::Assign(loc, move(constant), move(alias)));
    }
    return addUnderNesting(ctx, root, loc, perform.owner, name, move(body));
}

void addActiveJobMethods(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc,
                         const Perform &perform, const CopiedMethod &copied) {
    auto &file = *perform.owner.file;
    auto &params = copied.params;
    const Sig *sig = copied.sig ? &*copied.sig : nullptr;
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
    addCopiedMethod(ctx, body, loc, "perform_now", copied, true);
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

        auto copied = readMethod(ctx, loc, perform->owner, perform->method, join(perform->owner.fullName) + "#perform");
        if (!copied) {
            continue;
        }
        const Sig *sigPtr = copied->sig ? &*copied->sig : nullptr;

        if (perform->own) {
            if (kind == JobKind::Sidekiq) {
                addSidekiqMethods(ctx, tree->rhs, loc, *perform, copied->params, sigPtr, nullopt);
            } else {
                addActiveJobMethods(ctx, tree->rhs, loc, *perform, *copied);
            }
            continue;
        }
        // perform is in another file (a module the job includes, or a superclass). The methods are still the job's
        // own: Sorbet ranks the module Sidekiq::Job's `mixes_in_class_methods` adds above any module the job extends,
        // whatever the order. Their types are aliases inside that file's nesting, so the sig's constants resolve as
        // they do there, written once, in the job's own file.
        if (!main || main->file->path != file->path) {
            continue;
        }
        auto job = join(def.fullName);
        auto aliases = addTypeAliases(ctx, root, loc, *perform, copied->params, sigPtr,
                                      "SorbetSidekiqTypes_" + replaceAll(job, "::", "__"));
        if (aliases) {
            addSidekiqMethods(ctx, tree->rhs, loc, *perform, copied->params, sigPtr, aliases);
        }
    }
}

} // namespace sorbet::plugins::rails
