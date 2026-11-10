# typed: true
# Two included modules define perform: Ruby calls the last one included.
class TwoModulesJob
  include Sidekiq::Job
  include FirstPerform
  include SecondPerform
end

TwoModulesJob.perform_async(1)
TwoModulesJob.perform_async('x') # error: Expected `Integer` but found `String("x")` for argument `id`
