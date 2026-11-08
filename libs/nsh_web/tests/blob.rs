#[test]
fn negative_blob_slice_shares_original_bytes() {
    let b = Blob::new(b"abcdef".to_vec(), "text/plain".into());
    let s = b.slice(Some(-3.0), Some(-1.0), "text/custom".into());
    assert_eq!(s.bytes(), b"de");
    assert_eq!(s.mime, "text/custom");
    assert_eq!(s.bytes().as_ptr(), unsafe { b.bytes().as_ptr().add(3) });
}
