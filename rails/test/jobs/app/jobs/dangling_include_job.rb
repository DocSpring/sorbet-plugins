# typed: true
class DanglingIncludeJob
  include Sidekiq::Job
  include DanglingPerform
end

DanglingIncludeJob.perform_async(1)
