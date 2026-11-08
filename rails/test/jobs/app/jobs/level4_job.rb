# typed: true
# Five steps to ApplicationJob: past the plugin's limit, so it's a Sidekiq job (the default).
class Level4Job < Level3Job
  def perform(id); end
end

T.reveal_type(Level4Job.perform_async(1)) # error: Revealed type: `String`
