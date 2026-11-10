SET statement_timeout = 0;

CREATE TABLE public.photos (
    id bigint NOT NULL,
    type character varying,
    image_data text,
    thumbnail_data text
);

CREATE TABLE public.albums (
    id bigint NOT NULL,
    cover_data text
);

CREATE TABLE public.galleries (
    id bigint NOT NULL
);

CREATE TABLE public.books (
    id bigint NOT NULL,
    cover_data text
);

CREATE TABLE public.magazines (
    id bigint NOT NULL,
    cover_data text
);

CREATE TABLE public.twins (
    id bigint NOT NULL,
    image_data text
);
