# typed: true
# Neither kind.
class OrphanJob # error: rails plugin: can't tell what kind of job `OrphanJob` is: `OrphanJob` neither includes Sidekiq::Job nor has a superclass
  def perform(id); end
end
