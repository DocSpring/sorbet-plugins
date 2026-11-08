// Lineage: whether a class is a controller or a mailer, from its superclass chain, and its app parents.

#include "Rails.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_LINEAGE_DEPTH = 10;

} // namespace

bool endsWith(string_view text, string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

Lineage lineage(const App &app, const Definition &def) {
    Lineage out;
    const Definition *current = &def;
    for (int depth = 0; depth <= MAX_LINEAGE_DEPTH; depth++) {
        auto superclass = constantRef(*current->file, current->superclass);
        if (!superclass) {
            return {};
        }
        auto name = join(superclass->name);
        if (name == "ActionController::Base" || name == "ActionMailer::Base") {
            out.kind = name == "ActionController::Base" ? Kind::Controller : Kind::Mailer;
            out.directBase = true;
            return out;
        }
        if (name == "ActionController::API" || name == "ActionController::Metal") {
            return {};
        }
        auto parent = resolve(app, current->outerNesting(), *superclass);
        if (!parent) {
            // A gem's class: Devise::SessionsController, Doorkeeper::AuthorizationsController, Devise::Mailer.
            auto &last = superclass->name.back();
            out.kind = endsWith(last, "Controller") ? Kind::Controller
                       : endsWith(last, "Mailer")   ? Kind::Mailer
                                                    : Kind::None;
            return out.kind == Kind::None ? Lineage{} : out;
        }
        if (!parent->isClass) {
            return {};
        }
        out.parents.emplace_back(*parent);
        current = &out.parents.back();
    }
    return {};
}

} // namespace sorbet::plugins::rails
