# typed: true
class ApplicationController < ActionController::Base
  extend T::Sig
  include AdminAuth

  helper_method :string_param

  sig { params(key: T.any(String, Symbol)).returns(T.nilable(String)) }
  def string_param(key) = nil
end
