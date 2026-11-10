# typed: true
# Sidekiq::Job through a module of ours.
class ViaModuleJob
  include SidekiqBase
  extend T::Sig

  sig { params(id: Integer).void }
  def perform(id); end
end

ViaModuleJob.perform_async(1)
ViaModuleJob.perform_async('x') # error: Expected `Integer` but found `String("x")` for argument `id`
