SET statement_timeout = 0;

CREATE TABLE public.widgets (
    id bigint NOT NULL,
    name character varying,
    "order" integer DEFAULT 0 NOT NULL,
    status integer DEFAULT 0 NOT NULL,
    stage integer,
    color integer,
    size integer,
    mode integer,
    shade character varying,
    tone integer,
    finish integer,
    rank integer,
    flavor character varying,
    texture integer,
    settings jsonb,
    a integer,
    b integer,
    c integer,
    d integer,
    CONSTRAINT widgets_name_length CHECK ((length((name)::text) <= 255))
);

CREATE SEQUENCE public.widgets_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;

CREATE TABLE public.samples (
    id bigint NOT NULL,
    title character varying NOT NULL,
    body text,
    token uuid,
    digest bytea,
    count integer NOT NULL,
    small smallint,
    big bigint,
    total numeric(40,0),
    price numeric(20,10),
    plain numeric,
    ratio double precision NOT NULL,
    active boolean DEFAULT false NOT NULL,
    flagged boolean,
    created_at timestamp(6) without time zone NOT NULL,
    seen_at timestamp without time zone,
    born_on date,
    ip inet,
    span interval,
    payload jsonb DEFAULT '{}'::jsonb NOT NULL,
    extra json,
    tags character varying[] DEFAULT '{}'::character varying[] NOT NULL,
    notes text[],
    kind character varying,
    level integer,
    tier integer DEFAULT 0 NOT NULL,
    industries text,
    options text,
    blob text,
    old_column character varying,
    amount numeric(40,0) NOT NULL,
    retyped character varying
);

CREATE TABLE public.oddities (
    id bigint NOT NULL,
    cost money
);

CREATE TABLE public.oauth_applications (
    id uuid DEFAULT gen_random_uuid() NOT NULL,
    name character varying NOT NULL
);

CREATE TABLE public.users_roles (
    user_id bigint,
    role_id bigint
);

CREATE TABLE public.people (
    id integer NOT NULL
);

CREATE TABLE public.categories (
    id bigint NOT NULL
);

CREATE TABLE public.cacti (
    id bigint NOT NULL
);

CREATE TABLE public.gear (
    id bigint NOT NULL
);

CREATE TABLE public.shop_items (
    id bigint NOT NULL
);

CREATE TABLE public.pairs (
    left_id bigint NOT NULL,
    right_id bigint NOT NULL
);

CREATE TABLE public.audits (
    id bigint NOT NULL
);

CREATE TABLE public.posts (
    id bigint NOT NULL
);

CREATE TABLE public.comments (
    id bigint NOT NULL
);

CREATE TABLE public.tags (
    id bigint NOT NULL
);

CREATE TABLE public.items (
    id bigint NOT NULL
);

CREATE TABLE public.carts (
    id bigint NOT NULL
);

CREATE TABLE public.shop_carts (
    id bigint NOT NULL
);

ALTER TABLE ONLY public.widgets
    ADD CONSTRAINT widgets_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.samples
    ADD CONSTRAINT samples_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.oddities
    ADD CONSTRAINT oddities_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.oauth_applications
    ADD CONSTRAINT oauth_applications_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.people
    ADD CONSTRAINT people_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.categories
    ADD CONSTRAINT categories_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.cacti
    ADD CONSTRAINT cacti_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.gear
    ADD CONSTRAINT gear_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.shop_items
    ADD CONSTRAINT shop_items_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.pairs
    ADD CONSTRAINT pairs_pkey PRIMARY KEY (left_id, right_id);

ALTER TABLE ONLY public.audits
    ADD CONSTRAINT audits_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.posts
    ADD CONSTRAINT posts_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.comments
    ADD CONSTRAINT comments_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.tags
    ADD CONSTRAINT tags_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.items
    ADD CONSTRAINT items_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.carts
    ADD CONSTRAINT carts_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.shop_carts
    ADD CONSTRAINT shop_carts_pkey PRIMARY KEY (id);
