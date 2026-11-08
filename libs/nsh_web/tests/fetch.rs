#[test]
fn data_url_decode_preserves_binary_and_media_type() {
    assert_eq!(data_url("data:application/octet-stream,%00%FF"),
        Some((vec![0, 255], "application/octet-stream".into())));
}
