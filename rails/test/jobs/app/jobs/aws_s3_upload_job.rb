# typed: true
class AwsS3UploadJob
  include Sidekiq::Job
  include IntegrationUpload
end

AwsS3UploadJob.perform_async('Submission', 1)
AwsS3UploadJob.perform_async(1, 1) # error: Expected `String` but found `Integer(1)` for argument `action_klass`
