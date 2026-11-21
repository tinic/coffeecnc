// The page itself never zooms. On the controller's touch screen WebKitGTK turns a two-finger
// pinch that the page leaves unhandled into a zoom of the whole panel (turo: "it zooms the entire
// browser content instead of just the preview"). Marking every multi-touch event as handled stops
// that; the preview still sees the pinch as pointer events and zooms itself, and one-finger
// scrolling is untouched.
export function preventPageZoom(target: Pick<Document, 'addEventListener'> = document): void {
  const multiTouch = (event: Event): void => {
    if ((event as TouchEvent).touches.length > 1) event.preventDefault()
  }
  target.addEventListener('touchstart', multiTouch, { passive: false })
  target.addEventListener('touchmove', multiTouch, { passive: false })
  // WebKit's own pinch gesture events, where they exist.
  target.addEventListener('gesturestart', (event) => { event.preventDefault() }, { passive: false })
}
