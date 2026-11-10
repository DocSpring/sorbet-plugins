# typed: true
class Checks
  extend T::Sig

  sig { params(h: ApplicationController::HelperMethods).void }
  def application(h)
    T.reveal_type(h.string_param(:x)) # error: Revealed type: `T.nilable(String)`
    T.reveal_type(h.page_size) # error: Revealed type: `Integer`
    h.string_param(1) # error: Expected `T.any(String, Symbol)` but found `Integer(1)` for argument `key`
    T.reveal_type(h.staff_session?) # error: Revealed type: `T::Boolean`
    T.reveal_type(h.staff_session) # error: Revealed type: `T.nilable(AdminAuth::Session)`
    # helper :all, with ApplicationHelper (the default helper) first
    h.app_helper_method
    h.about_helper_method
    h.oauth_helper_method
    h.admin_views_helper_method
    h.mailer_helper_method
    # The app's helper wins over the gem module's, as in Rails.
    T.reveal_type(h.notice) # error: Revealed type: `Integer`
  end

  sig { params(h: DashboardController::HelperMethods).void }
  def dashboard(h)
    h.string_param(:x)
    h.app_helper_method
    T.reveal_type(h.period_dates) # error: Revealed type: `String`
    T.reveal_type(h.filter_params) # error: Revealed type: `T::Hash[Symbol, T.untyped]`
    h.filter_params(1) # error: Expected `T::Hash[Symbol, T.untyped]` but found `Integer(1)` for argument `overrides`
    T.reveal_type(h.untyped_one(1, b: 2)) # error: Revealed type: `T.untyped`
    h.untyped_one # error: Not enough arguments provided for method `DashboardController::HelperMethods#untyped_one`
    h.reopened_helper
  end

  sig { params(c: DashboardController).void }
  def proxy(c)
    T.reveal_type(c.helpers) # error: Revealed type: `DashboardController::HelperProxy`
    T.reveal_type(c.helpers.period_dates) # error: Revealed type: `String`
    c.helpers.content_tag(:div)
    c.helpers.string_param(:x)
  end

  sig { params(h: StaticPageController::HelperMethods).void }
  def static_page(h)
    T.reveal_type(h.plans) # error: Revealed type: `T::Array[HomeController::Plan]`
    h.string_param(:x)
  end

  sig { params(h: RobotsController::HelperMethods).void }
  def robots(h)
    h.app_helper_method
    h.about_helper_method
    h.admin_views_helper_method
    h.string_param(:x) # error: Method `string_param` does not exist on `RobotsController::HelperMethods`
  end

  sig { params(h: OAuth::AuthorizationsController::HelperMethods, c: OAuth::AuthorizationsController).void }
  def gem_parent(h, c)
    T.reveal_type(h.oauth_application) # error: Revealed type: `T.nilable(String)`
    c.helpers.oauth_application
  end

  sig { params(c: CSPReportsController).void }
  def api(c)
    c.helpers # error: Method `helpers` does not exist on `CSPReportsController`
  end

  sig { params(h: FailureAlertMailer::HelperMethods, m: FailureAlertMailer).void }
  def mailer(h, m)
    T.reveal_type(h.failure_time(Time.now, show_zone: true)) # error: Revealed type: `String`
    h.failure_time('now') # error: Expected `Time` but found `String("now")` for argument `time`
    T.reveal_type(h.logo_url) # error: Revealed type: `String`
    h.mailer_helper_method
    h.app_helper_method # error: Method `app_helper_method` does not exist on `FailureAlertMailer::HelperMethods`
    m.helpers # error: Method `helpers` does not exist on `FailureAlertMailer`
  end

  sig { params(h: AccountMailer::HelperMethods).void }
  def mailer_without_helpers(h)
    h.logo_url
    h.mailer_helper_method
  end
end

ApplicationMailer::AbortDeliveryError::HelperMethods # error: Unable to resolve constant `HelperMethods`
CSPReportsController::HelperMethods # error: Unable to resolve constant `HelperMethods`
