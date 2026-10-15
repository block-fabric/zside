import { mount } from 'svelte';
import './styles/fonts.css';
import './styles/themes.css';
import './styles/base.css';
import './styles/side.css';
import App from './App.svelte';
import { CHAIN } from './chain/config';

// The colour of the mark of the chain (components/Brand.svelte), set through the CSS object model.
if (CHAIN.brand.color) document.documentElement.style.setProperty('--brand', CHAIN.brand.color);

const target = document.getElementById('app');
if (!target) throw new Error('no #app element');

export default mount(App, { target });
