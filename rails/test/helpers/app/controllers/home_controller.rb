# typed: true
class HomeController < ApplicationController
  extend T::Sig

  class Plan; end

  # `Plan` is HomeController::Plan: a subclass's helper_method copies this sig in HomeController's scope.
  sig { returns(T::Array[Plan]) }
  def plans = []
end
