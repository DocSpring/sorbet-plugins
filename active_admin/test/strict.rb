# typed: strict

# Action blocks become methods with an implied `sig { void }`, so a strict file needs no sig for them. Methods
# written in `controller do` still need their own.
ActiveAdmin.register User do
  member_action :lock, method: :post do
    resource.name
    redirect_to '/'
  end

  collection_action :export do
    redirect_to '/'
  end

  batch_action :destroy do |ids|
    redirect_to '/'
  end

  controller do
    def check # error: The method `check` does not have a `sig`
      redirect_to '/'
    end
  end
end

ActiveAdmin.register_page 'Dashboard' do
  page_action :refresh, method: :post do
    redirect_to '/'
  end
end
