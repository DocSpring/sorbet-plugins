# typed: true
# Associations: the methods Tapioca wrote for each kind, typed with the class Rails finds for it.
class Post < ApplicationRecord
  include HasAuthor

  # The same as the concern's: fine whichever runs first.
  belongs_to :author, class_name: 'Person'
  belongs_to :category
  belongs_to :subject, polymorphic: true, optional: true
  belongs_to :audit_record, class_name: 'Audited::Audit'
  has_one :gear, -> { where(id: 1) }, inverse_of: false
  has_one :pinned_comment, class_name: :Comment
  has_one :first_commenter, through: :pinned_comment, source: :person
  has_many :comments, dependent: :destroy do
    def recent; end
  end
  has_many :cacti
  has_many :commenters, through: :comments, source: :person
  has_many :people, through: :comments
  has_many :subjects, through: :comments, source_type: 'Widget'
  has_and_belongs_to_many :tags

  accepts_nested_attributes_for :comments, :category, allow_destroy: true
end

# belongs_to: the record, its reload and reset, the change predicates, and building one.
T.reveal_type(Post.new.category) # error: Revealed type: `T.nilable(Category)`
Post.new.category = Gear.new # error: Assigning a value to `value` that does not match expected type `T.nilable(Category)`
T.reveal_type(Post.new.reload_category) # error: Revealed type: `T.nilable(Category)`
T.reveal_type(Post.new.reset_category) # error: Revealed type: `Sorbet::Private::Static::Void`
T.reveal_type(Post.new.category_changed?) # error: Revealed type: `T::Boolean`
T.reveal_type(Post.new.category_previously_changed?) # error: Revealed type: `T::Boolean`
T.reveal_type(Post.new.build_category) # error: Revealed type: `Category`
T.reveal_type(Post.new.create_category(id: 1)) # error: Revealed type: `Category`
T.reveal_type(Post.new.create_category!) # error: Revealed type: `Category`

# A polymorphic one: untyped, and nothing to build.
T.reveal_type(Post.new.subject) # error: Revealed type: `T.untyped`
T.reveal_type(Post.new.subject_changed?) # error: Revealed type: `T::Boolean`
Post.new.build_subject # error: Method `build_subject` does not exist on `Post`

# A class that isn't in the app's files (a gem's model): written as given.
T.reveal_type(Post.new.audit_record) # error: Revealed type: `T.nilable(Audited::Audit)`

# has_one: no change predicates.
T.reveal_type(Post.new.gear) # error: Revealed type: `T.nilable(Gear)`
T.reveal_type(Post.new.build_gear) # error: Revealed type: `Gear`
Post.new.gear_changed? # error: Method `gear_changed?` does not exist on `Post`
T.reveal_type(Post.new.pinned_comment) # error: Revealed type: `T.nilable(Comment)`

# has_one through: the source's class, and building one.
T.reveal_type(Post.new.first_commenter) # error: Revealed type: `T.nilable(Person)`
T.reveal_type(Post.new.build_first_commenter) # error: Revealed type: `Person`

# has_many: the class's collection proxy, and the ids.
T.reveal_type(Post.new.comments) # error: Revealed type: `Comment::PrivateCollectionProxy`
Post.new.comments = [Gear.new] # error: Assigning a value to `value` that does not match expected type `T::Enumerable[Comment]`
T.reveal_type(Post.new.comment_ids) # error: Revealed type: `T::Array[T.untyped]`
Post.new.comment_ids = [1]
T.reveal_type(Post.new.cacti) # error: Revealed type: `Cactus::PrivateCollectionProxy`
T.reveal_type(Post.new.cactus_ids) # error: Revealed type: `T::Array[T.untyped]`

# has_many through: the source's class (named by `source:`, or the singular or plural of the name), and untyped for a
# polymorphic source.
T.reveal_type(Post.new.commenters) # error: Revealed type: `Person::PrivateCollectionProxy`
T.reveal_type(Post.new.people) # error: Revealed type: `Person::PrivateCollectionProxy`
T.reveal_type(Post.new.person_ids) # error: Revealed type: `T::Array[T.untyped]`
T.reveal_type(Post.new.subjects) # error: Revealed type: `ActiveRecord::Associations::CollectionProxy`
Post.new.subjects = [Gear.new]

# has_and_belongs_to_many.
T.reveal_type(Post.new.tags) # error: Revealed type: `Tag::PrivateCollectionProxy`
T.reveal_type(Post.new.tag_ids) # error: Revealed type: `T::Array[T.untyped]`

# From the concern's `included` block, which runs in Post: its change predicates are in Post's module.
T.reveal_type(Post.new.author) # error: Revealed type: `T.nilable(Person)`
T.reveal_type(T.let(T.unsafe(nil), Post::GeneratedAssociationMethods).author_changed?) # error: Revealed type: `T::Boolean`

# Nested attributes.
Post.new.comments_attributes = [{}]
Post.new.category_attributes = {}
Post.new.tags_attributes = [] # error: Setter method `tags_attributes=` does not exist on `Post`
