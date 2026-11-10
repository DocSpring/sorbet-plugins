# typed: true
# What Tapioca's narrowed compiler writes for ApplicationController: the helper modules it inherits from
# ActionController::Base, gems' included, which the plugin can't see. They go in the module the plugin includes
# first, since Sorbet orders a module's mixins from several files by path, putting these after the app's helpers.
class ApplicationController
  module SorbetInheritedHelperMethods
    include ::ActionController::Base::HelperMethods
  end
end
