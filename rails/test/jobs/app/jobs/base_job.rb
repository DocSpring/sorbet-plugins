# typed: true
# A decoy: DataMigrations::Export::BatchBaseJob's `BaseJob` is DataMigrations::Export::BaseJob, not this.
class BaseJob
  include Sidekiq::Job
end
