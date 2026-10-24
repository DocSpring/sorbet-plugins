# typed: true
# sorbet-args: --enable-experimental-rspec

# test-prof's `let_it_be` and `before_all` are checked as a `let` and a `before` hook.
RSpec.describe 'let_it_be' do
  let_it_be(:user) { User.all }
  let_it_be(:other, refind: true, freeze: false) { user }

  before_all do
    get '/'
    gt '/' # error: Method `gt` does not exist
  end

  it 'reads them like lets' do
    user
    other
    usr # error: Method `usr` does not exist
  end

  describe 'a nested group', let_it_be_modifiers: { refind: true } do
    let_it_be(:nested) { other }

    it 'reads the outer ones too' do
      nested
      user
      nestd # error: Method `nestd` does not exist
    end
  end
end
