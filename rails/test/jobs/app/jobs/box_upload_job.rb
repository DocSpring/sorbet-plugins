# typed: true
# perform from IntegrationUpload, through StorageProviderUpload.
class BoxUploadJob
  include Sidekiq::Job
  include StorageProviderUpload
end

T.reveal_type(BoxUploadJob.perform_async('Submission', 1)) # error: Revealed type: `String`
BoxUploadJob.perform_in(60, 'Submission', 'one') # error: Expected `Integer` but found `String("one")` for argument `action_id`
