# typed: true
# sorbet-args: --enable-experimental-rspec

RSpec.describe 'factories' do
  let(:user) { create(:user, :admin, name: 'Ada') }
  let!(:users) { create_list(:user, 2) }

  it 'types the records' do
    expect(user.admin?).to eq(true)
    user.nme # error: Method `nme` does not exist on `User`
    users.first&.nme # error: Method `nme` does not exist on `User`
    build(:user).name
    FactoryBot.create(:user).name
    attributes_for(:user).fetch(:name)
  end

  it 'checks factory, trait and attribute names' do
    create(:usr) # error: Unable to resolve constant `Usr`
    create(:user, :admn) # error: Method `admn` does not exist on `T.class_of(FactoryBotTypes::User::Traits)`
    create(:user, nme: 'x') # error: Unrecognized keyword argument `nme`
  end
end
