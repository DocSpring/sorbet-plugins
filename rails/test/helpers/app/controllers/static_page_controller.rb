# typed: true
class StaticPageController < HomeController
  helper_method :plans, :missing_method # error: rails plugin: can't find the method `helper_method :missing_method` names
end
