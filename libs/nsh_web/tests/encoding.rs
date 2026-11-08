#[test]
fn split_utf8_and_bom_wait_for_next_chunk() {
    let mut d = Decoder::new(Encoding::Utf8, false, false);
    assert_eq!(d.decode(b"\xef\xbb", true).ok().as_deref(), Some(""));
    assert_eq!(d.decode(b"\xbf\xf0\x9f", true).ok().as_deref(), Some(""));
    assert_eq!(d.decode(b"\x98\x80", false).ok().as_deref(), Some("😀"));
}
#[test]
fn fatal_utf8_rejects_invalid_sequences() {
    let mut d = Decoder::new(Encoding::Utf8, true, false);
    assert!(d.decode(b"\xff", false).is_err());
}
