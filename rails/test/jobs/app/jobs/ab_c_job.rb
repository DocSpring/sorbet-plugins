# typed: true
class AbCJob
  include Sidekiq::Job
  include AbC
end

AbCJob.perform_async(1)
AbCJob.perform_async('x') # error: Expected `Integer` but found `String("x")` for argument `id`
