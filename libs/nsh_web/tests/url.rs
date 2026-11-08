#[test]
fn form_query_decodes_plus_and_preserves_duplicates() {
    assert_eq!(parse_query("a=hello+world&a=%F0%9F%98%80&empty"), vec![
        ("a".into(), "hello world".into()), ("a".into(), "😀".into()), ("empty".into(), "".into())
    ]);
}
