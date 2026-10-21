# typed: true

ActiveAdmin.register User do
  menu if: proc { current_user }
  permit_params :name, :email
  filter :name
  filtr :email # error: Method `filtr` does not exist on `ActiveAdmin::ResourceDSL`

  controller do
    before_action :check

    def check
      redirect_to '/' unless params[:ok]
      resource.name
      resource.nme # error: Method `nme` does not exist on `User`
    end
  end

  before_create do |user|
    user.email
    user.emial # error: Method `emial` does not exist on `User`
  end

  member_action :lock, method: :post do
    resource.name
    redirect_to '/'
    resouce # error: Method `resouce` does not exist
  end

  index do
    column :name
    column(:email) { |user| user.email }
    column(:email) { |user| user.emial } # error: Method `emial` does not exist on `User`
    actions
  end

  show do |shown|
    attributes_table do
      row(:name) { |record| link_to record.name, '/' }
      row(:account) { |record| record.acount } # error: Method `acount` does not exist on `User`
    end
    panel 'Other' do
      table_for [1, 2] do
        column(:x) { |number| number.anything }
      end
    end
    shown.email
    user.name
    resource.nmae # error: Method `nmae` does not exist on `User`
  end

  form do |f|
    f.inputs do
      f.input :name
    end
    f.actons # error: Method `actons` does not exist on `ActiveAdmin::FormBuilder`
  end
end

unless ENV['ENTERPRISE']
  ActiveAdmin.register_page 'Dashboard' do
    content do
      para 'Hello'
      pra 'typo' # error: Method `pra` does not exist on `ActiveAdminTypes::Pages::Dashboard::View`
    end

    page_action :refresh, method: :post do
      redirect_to '/'
      resource # error: Method `resource` does not exist on `ActiveAdminTypes::Pages::Dashboard::Controller`
    end
  end
end
