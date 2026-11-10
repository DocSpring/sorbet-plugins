# typed: true
# Five steps to ApplicationJob: an ActiveJob job.
class Level4Job < Level3Job
  def perform(id); end
end

T.reveal_type(Level4Job.perform_later(1)) # error: Revealed type: `T.any(Level4Job, FalseClass)`
Level4Job.perform_async(1) # error: Method `perform_async` does not exist on `T.class_of(Level4Job)`
