# typed: true
# sorbet-args: --enable-experimental-rspec

RSpec.describe 'loops' do
  %w[a b].each do |name|
    label = name.upcase

    it "works for #{name}", :some_tag do
      expect(label).to eq(name.upcase)
      name.upcse # error: Method `upcse` does not exist on `String`
    end

    before(:each) { get "/#{name}" }
  end

  # test_each_hash. Sorbet types a hash literal's pairs as T.untyped, but the examples are still checked.
  { one: 1, two: 2 }.each do |key, value|
    context "with #{key}" do
      it { value.anything }
      it { gt '/' } # error: Method `gt` does not exist
    end
  end

  [['a', 1]].each do |letter, (number, _)|
    it { letter.upcase }
  end

  User.all.each do |user|
    it 'gets an untyped loop variable when test_each would raise' do
      user.anything
      gt '/' # error: Method `gt` does not exist
    end
  end

  %w[a].each do |outer|
    %w[b].each do |inner|
      it 'nests' do
        outer.upcase
        inner.anything
        gt '/' # error: Method `gt` does not exist
      end
    end
  end

  %i[get post].each do |verb|
    next if verb == :post

    it_behaves_like 'shared', verb
    it { verb.to_proc }
  end
end
