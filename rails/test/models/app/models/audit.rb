# typed: true
# A gem's model as the superclass: Tapioca keeps writing its methods.
class Audit < Audited::Audit
end

Audit.ids # error: Method `ids` does not exist on `T.class_of(Audit)`
