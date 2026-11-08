# typed: true
class ArchiveJob
  include Sidekiq::Job
  include Uploads::Shared

  class Kind; end
end

ArchiveJob.perform_async(Uploads::Kind.new)
ArchiveJob.perform_async(ArchiveJob::Kind.new) # error: Expected `Uploads::Kind` but found `ArchiveJob::Kind` for argument `kind`
