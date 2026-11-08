# typed: true
class DashboardController < ApplicationController
  extend T::Sig

  helper_method :period_dates, 'filter_params', :untyped_one

  private

  sig { returns(String) }
  def period_dates = ''

  sig { params(overrides: T::Hash[Symbol, T.untyped]).returns(T::Hash[Symbol, T.untyped]) }
  def filter_params(overrides = {}) = overrides

  def untyped_one(a, b: 1); end
end
