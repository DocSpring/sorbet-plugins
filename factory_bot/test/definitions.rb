# typed: true

FactoryBot.define do
  sequence(:email) { |n| "user#{n}@example.com" }

  factory :user do
    name { "User #{nickname} #{admin?}" }
    admin { false }
    nmae { 'Ann' } # error: Method `nmae` does not exist on `FactoryBotTypes::User::Definition`

    transient do
      nickname { 'Ann' }
    end

    trait :admin do
      admin { nickname.present? && admn? } # error: Method `admn?` does not exist on `FactoryBotTypes::User::Evaluator`
    end

    after(:create) do |user, evaluator|
      user.admin?
      evaluator.nickname
      evaluator.nickame # error: Method `nickame` does not exist on `FactoryBotTypes::User::Evaluator`
      create(:user, :admin).name
    end

    factory :admin_user do
      admin { true }
      nickname { 'x' } # error: Method `nickname` does not exist on `FactoryBotTypes::AdminUser::Definition`
    end
  end

  factory :post do # error: Unable to resolve constant `Post`
    title { 'x' }
  end
end
