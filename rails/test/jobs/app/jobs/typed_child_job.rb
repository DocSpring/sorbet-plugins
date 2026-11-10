# typed: true
# The parent's perform: its sig's constants resolve as they do in the parent, through the parent's ancestors.
class TypedChildJob < TypedParentJob
end

TypedChildJob.perform_async(1)
TypedChildJob.perform_async('x') # error: Expected `Integer` but found `String("x")` for argument `id`
