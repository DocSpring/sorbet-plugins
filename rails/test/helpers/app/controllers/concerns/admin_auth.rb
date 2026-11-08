# typed: true
module AdminAuth
  extend ActiveSupport::Concern
  extend T::Sig

  class Session; end

  included do
    T.bind(self, T.class_of(ApplicationController))
    helper_method :staff_session?, :staff_session
  end

  sig { returns(T::Boolean) }
  def staff_session? = true

  # `Session` is AdminAuth::Session here: the copied sig must resolve in this module's scope.
  sig { returns(T.nilable(Session)) }
  def staff_session = nil
end
