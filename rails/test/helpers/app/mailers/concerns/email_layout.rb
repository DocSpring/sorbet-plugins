# typed: true
module EmailLayout
  extend ActiveSupport::Concern
  extend T::Sig

  included do
    T.bind(self, T.class_of(ActionMailer::Base))
    helper_method :logo_url
  end

  sig { returns(String) }
  def logo_url = ''
end
