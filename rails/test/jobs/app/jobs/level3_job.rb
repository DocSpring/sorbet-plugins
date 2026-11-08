# typed: true
# Four steps to ApplicationJob: an ActiveJob job.
class Level3Job < Level2Job
  def perform(id); end
end

T.reveal_type(Level3Job.perform_now(1)) # error: Revealed type: `T.untyped`
