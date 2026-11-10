# typed: true
# An app helper with a method that a gem helper module also defines: Rails calls this one, since a class's helpers
# include its parent's module (ActionController::Base's, with the gems' helpers) before the app's.
module NoticeHelper
  extend T::Sig

  sig { returns(Integer) }
  def notice = 1
end
