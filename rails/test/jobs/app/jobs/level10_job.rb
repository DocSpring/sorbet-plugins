# typed: true
# Eleven steps to ApplicationJob: past the plugin's limit, so it says it can't tell rather than guess.
class Level10Job < Level9Job # error: rails plugin: can't tell what kind of job `Level10Job` is: it's more than 10 superclasses from ApplicationJob or a Sidekiq::Job include
  def perform(id); end
end
