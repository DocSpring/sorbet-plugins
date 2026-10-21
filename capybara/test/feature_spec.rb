# typed: true
# sorbet-args: --enable-experimental-rspec

RSpec.feature 'Signing in' do
  given(:user) { User.new }
  given!(:other) { User.new }

  background do
    visit '/'
  end

  scenario 'with a name' do
    click_on user.name
    expect(other).to have_content('Welcome')
  end

  xscenario 'pending' do
    visit '/pending'
  end

  feature 'nested' do
    scenario 'sees the name' do
      User.new.nmae # error: Method `nmae` does not exist on `User`
      vist '/' # error: Method `vist` does not exist
    end
  end
end
