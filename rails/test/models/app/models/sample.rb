# typed: true
# Columns: a method set for each column of the table and each `attribute`, with types from the column's SQL type and
# the macros that change it, in the order they run.
class Sample < ApplicationRecord
  include HasLabels

  self.ignored_columns += ['old_column']

  enum :kind, { basic: 'basic', fancy: 'fancy' }
  enum :level, { low: 0, high: 1 }
  attr_enum :tier, { free: 0, paid: 1 }
  attr_enum :plan, { monthly: 0, yearly: 1 }
  serialize :industries, type: Array
  serialize :options, type: Hash
  serialize :blob, coder: JSON
  attribute :amount, :decimal, precision: 40
  attribute :virtual_flag, :boolean
  attribute :anything
  attribute :when, :datetime
  attribute :codes, :string, array: true
  attribute :retyped, :integer
  attribute :retyped
  default_boolean_value :flagged, :extra_flag, value: true
  alias_attribute :heading, :title
  labelled :caption
  serialize :options_list, type: Set
end

sample = Sample.new

# Each SQL type, with NOT NULL columns not nilable.
T.reveal_type(sample.id) # error: Revealed type: `Integer`
T.reveal_type(sample.title) # error: Revealed type: `String`
T.reveal_type(sample.body) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.token) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.digest) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.count) # error: Revealed type: `Integer`
T.reveal_type(sample.small) # error: Revealed type: `T.nilable(Integer)`
T.reveal_type(sample.big) # error: Revealed type: `T.nilable(Integer)`
T.reveal_type(sample.total) # error: Revealed type: `T.nilable(Integer)`
T.reveal_type(sample.price) # error: Revealed type: `T.nilable(BigDecimal)`
T.reveal_type(sample.plain) # error: Revealed type: `T.nilable(BigDecimal)`
T.reveal_type(sample.ratio) # error: Revealed type: `Float`
T.reveal_type(sample.active) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.created_at) # error: Revealed type: `ActiveSupport::TimeWithZone`
T.reveal_type(sample.seen_at) # error: Revealed type: `T.nilable(ActiveSupport::TimeWithZone)`
T.reveal_type(sample.born_on) # error: Revealed type: `T.nilable(Date)`
T.reveal_type(sample.ip) # error: Revealed type: `T.nilable(IPAddr)`
T.reveal_type(sample.span) # error: Revealed type: `T.nilable(ActiveSupport::Duration)`
T.reveal_type(sample.payload) # error: Revealed type: `T.untyped`
T.reveal_type(sample.extra) # error: Revealed type: `T.untyped`
T.reveal_type(sample.tags) # error: Revealed type: `T::Array[String]`
T.reveal_type(sample.notes) # error: Revealed type: `T.nilable(T::Array[String])`

# Enums: the getter is a String; the setter takes a Symbol too, and an Integer when the enum's type is an integer.
T.reveal_type(sample.kind) # error: Revealed type: `T.nilable(String)`
sample.kind = 1 # error: Assigning a value to `value` that does not match expected type `T.nilable(T.any(String, Symbol))`
T.reveal_type(sample.level) # error: Revealed type: `T.nilable(String)`
sample.level = 1.5 # error: Assigning a value to `value` that does not match expected type `T.nilable(T.any(String, Symbol, Integer))`
T.reveal_type(sample.tier) # error: Revealed type: `String`
# `attr_enum` declares the attribute, so an enum needs no column.
T.reveal_type(sample.plan) # error: Revealed type: `String`
sample.plan = nil # error: Assigning a value to `value` that does not match expected type `T.any(String, Symbol, Integer)`

# Serialized columns: YAML arrays and hashes keep their class and aren't nilable.
T.reveal_type(sample.industries) # error: Revealed type: `T::Array[T.untyped]`
sample.industries = nil
T.reveal_type(sample.options) # error: Revealed type: `T::Hash[T.untyped, T.untyped]`
T.reveal_type(sample.blob) # error: Revealed type: `T.untyped`
# Of another class, untyped; on a name without a column, a new attribute.
T.reveal_type(sample.options_list) # error: Revealed type: `T.untyped`

# `attribute`: its type over the column's, and a virtual attribute isn't nilable.
T.reveal_type(sample.amount) # error: Revealed type: `BigDecimal`
T.reveal_type(sample.virtual_flag) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.anything) # error: Revealed type: `T.untyped`
T.reveal_type(sample.when) # error: Revealed type: `ActiveSupport::TimeWithZone`
T.reveal_type(sample.codes) # error: Revealed type: `T::Array[String]`
# Without a type, `attribute` keeps the one before it.
T.reveal_type(sample.retyped) # error: Revealed type: `T.nilable(Integer)`
T.reveal_type(sample.flagged) # error: Revealed type: `T.nilable(T::Boolean)`
T.reveal_type(sample.extra_flag) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.caption) # error: Revealed type: `String`

# Ignored columns have no methods.
sample.old_column # error: Method `old_column` does not exist on `Sample`

# Aliases: Rails' `id_value`, and the app's.
T.reveal_type(sample.id_value) # error: Revealed type: `Integer`
T.reveal_type(sample.heading) # error: Revealed type: `String`
T.reveal_type(sample.heading_was) # error: Revealed type: `T.nilable(String)`

# The rest of each attribute's methods.
sample.title = 'x'
T.reveal_type(sample.title?) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.title_before_last_save) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.title_change_to_be_saved) # error: Revealed type: `T.nilable([String, String])`
T.reveal_type(sample.title_in_database) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.saved_change_to_title) # error: Revealed type: `T.nilable([String, String])`
T.reveal_type(sample.saved_change_to_title?(from: 'a', to: 'b')) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.will_save_change_to_title?) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.title_change) # error: Revealed type: `T.nilable([String, String])`
T.reveal_type(sample.title_changed?(from: 'a')) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.title_will_change!) # error: Revealed type: `Sorbet::Private::Static::Void`
T.reveal_type(sample.title_was) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.title_previous_change) # error: Revealed type: `T.nilable([String, String])`
T.reveal_type(sample.title_previously_changed?(to: 'b')) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.title_previously_was) # error: Revealed type: `T.nilable(String)`
T.reveal_type(sample.restore_title!) # error: Revealed type: `Sorbet::Private::Static::Void`
T.reveal_type(sample.title_before_type_cast) # error: Revealed type: `T.untyped`
T.reveal_type(sample.title_came_from_user?) # error: Revealed type: `T::Boolean`
T.reveal_type(sample.payload_was) # error: Revealed type: `T.untyped`
