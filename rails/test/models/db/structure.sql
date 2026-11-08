SET statement_timeout = 0;

CREATE TABLE public.widgets (
    id bigint NOT NULL,
    name character varying,
    "order" integer DEFAULT 0 NOT NULL,
    CONSTRAINT widgets_name_length CHECK ((length((name)::text) <= 255))
);

CREATE SEQUENCE public.widgets_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;

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

ALTER TABLE ONLY public.widgets
    ADD CONSTRAINT widgets_pkey PRIMARY KEY (id);

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
