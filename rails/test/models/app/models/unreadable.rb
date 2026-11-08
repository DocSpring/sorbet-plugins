# typed: true
# Macros the plugin can't read: an error at each, and the rest are still written.
class Unreadable < ApplicationRecord # error: app/models/concerns/bad_macros.rb line 9: can't read this `scope`
  include BadMacros

  self.table_name = 'widgets'

  def self.computed; end

  name = :x
  scope name, -> {} # error: can't read this `scope`: the plugin reads a scope whose name is a symbol or string
  enum :a, computed # error: can't read the values of `enum :a`
  enum :b, MISSING # error: Unable to resolve constant `MISSING` # error: can't find the constant `MISSING` (the values of `enum :b`)
  enum :c, { 'not a name': 0 } # error: `not a name` isn't a method name
  enum :d, { x: 0 }, prefix: computed # error: can't read `prefix:` of `enum :d`
  with_options({}) do
    scope :nested, -> {} # error: can't read `scope` here: the plugin reads it only directly in a class body
    attribute :nested_attribute, :string # error: can't read `attribute` here: the plugin reads it only directly in a class body
    has_many :nested_association # error: can't read `has_many` here: the plugin reads it only directly in a class body
  end
  scope :fine, -> {}

  attribute name # error: can't read this `attribute`: the plugin reads a name that's a symbol or string
  attribute :typed, ActiveRecord::Type::Binary.new # error: can't read the type of `attribute :typed`: the plugin reads a type that's a symbol (`:boolean`)
  attribute :money, :money # error: doesn't type `attribute :money, :money`
  serialize :coded, coder: computed # error: can't read `coder:` of `serialize :coded`
  alias_attribute :other, computed # error: can't read this `alias_attribute`
  self.ignored_columns += computed # error: can't read `self.ignored_columns +=`
  text_attribute :note # error: can't read `text_attribute`'s definition: it calls `attribute`
  attribute :fine_column, :integer
  enum :ghost, { x: 0 } # error: `enum :ghost` has no column or type

  has_many name # error: can't read this `has_many`: the plugin reads an association whose name is a symbol or string
  belongs_to :computed_class, class_name: computed # error: can't read `class_name:` of `belongs_to :computed_class`: the plugin reads a string or symbol
  belongs_to :computed_polymorphic, polymorphic: computed # error: can't read `polymorphic:` of `belongs_to :computed_polymorphic`: the plugin reads `true` or `false`
  has_many :splatted, **{} # error: can't read the options of `has_many :splatted`: the plugin reads `key: value` options
  has_many :anonymous, anonymous_class: Category # error: doesn't type `anonymous_class:` (`has_many :anonymous`)
  has_versions # error: can't read `has_versions`'s definition: it calls `has_many`
  accepts_nested_attributes_for computed # error: can't read this `accepts_nested_attributes_for`: the plugin reads names that are symbols or strings
  belongs_to :palette # error: can't type `belongs_to :palette`: Rails finds the module `Palette`, which isn't a model
  has_many :comments, as: :subject
  belongs_to :subject, polymorphic: true
  belongs_to :audit_record, class_name: 'Audited::Audit'
  belongs_to :shop_cart, class_name: 'Shop::Cart'
  has_many :lost, through: :missing # error: can't type `has_many :lost`: `Unreadable` has no association `missing` (its `through:`)
  has_many :readers, through: :comments # error: can't type `has_many :readers`: `Comment` has no association `reader` or `readers` (the source of `through: :comments`)
  has_many :sourced, through: :comments, source: :nothing # error: can't type `has_many :sourced`: `Comment` has no association `nothing` (the source of `through: :comments`)
  has_many :subject_posts, through: :subject, source: :post # error: can't type `has_many :subject_posts`: its `through:` association `subject` is polymorphic
  has_many :audit_users, through: :audit_record # error: can't type `has_many :audit_users`: its `through:` association `audit_record` has the class `Audited::Audit`, which isn't a model in the app's files
  has_many :items, through: :shop_cart # error: can't type `has_many :items`: `Shop::Cart` has both `item` and `items`, so Rails can't tell which is the source of `through: :shop_cart`
  has_many :subjects, through: :comments # error: can't type `has_many :subjects`: its source `Comment#subject` is polymorphic, and Rails needs `source_type:` for it
  has_many :fine_comments, class_name: 'Comment'
end

T.reveal_type(Unreadable.fine) # error: Revealed type: `Unreadable::PrivateRelation`
T.reveal_type(Unreadable.new.fine_column) # error: Revealed type: `Integer`
T.reveal_type(Unreadable.new.name) # error: Revealed type: `T.nilable(String)`
T.reveal_type(Unreadable.new.fine_comments) # error: Revealed type: `Comment::PrivateCollectionProxy`
Unreadable.new.lost # error: Method `lost` does not exist on `Unreadable`
