# typed: true
# sorbet-args: --enable-experimental-rspec

# The path makes this a request spec: its groups get RSpecConfig::Everywhere and RSpecConfig::Type::Request.
RSpec.describe 'groups' do
  it 'has the request helpers' do
    get '/'
    expect(1).to eq(1)
    model_helper # error: Method `model_helper` does not exist
  end

  describe 'a model group', type: :model do
    it 'adds the type modules' do
      model_helper
      get '/'
    end
  end

  shared_examples 'shared' do
    it 'gets the modules too' do
      expect(get('/')).to eq(nil)
      gt '/' # error: Method `gt` does not exist
    end
  end

  if ENV['CI']
    it 'is checked inside a class-level condition' do
      expect(1).to eq(1)
      gt '/' # error: Method `gt` does not exist
    end
  end

  it 'types the example param' do |example|
    example.metadata
    example.metdata # error: Method `metdata` does not exist on `RSpec::Core::Example`
  end

  around do |example|
    example.run
    example.rn # error: Method `rn` does not exist on `RSpec::Core::Example::Procsy`
  end
end
