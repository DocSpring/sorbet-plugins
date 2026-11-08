# typed: true
# perform comes from the parent: the parent's perform_async types it.
class InheritingJob < ParentJob
end

InheritingJob.perform_async(1)
InheritingJob.perform_async('x') # error: Expected `Integer` but found `String("x")` for argument `id`
