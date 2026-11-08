# typed: true
# No perform: not a job.
class NotAJob
end

NotAJob.perform_async # error: Method `perform_async` does not exist on `T.class_of(NotAJob)`
