# typed: true
# Reopens DashboardController outside its Zeitwerk file: its helpers come from dashboard_controller.rb alone.
class DashboardController
  helper_method :reopened_helper

  def reopened_helper; end
end
